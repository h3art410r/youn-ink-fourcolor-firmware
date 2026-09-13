#include "weather_api.h"
#include "settings.h"

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <miniz.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr char kTag[] = "WeatherApi";
constexpr int64_t kRefreshIntervalUs = 3600LL * 1000000LL;
constexpr int64_t kRetryIntervalUs = 60LL * 1000000LL;
constexpr char kForecastUrl[] =
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
    "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=4"
    "&wind_speed_unit=ms&timezone=Asia%%2FShanghai";

char s_city_name[32] = {};
char s_provider[16] = "open-meteo";
char s_qweather_host[128] = {};
char s_qweather_credential[384] = {};
double s_latitude = 0;
double s_longitude = 0;
WeatherCallback s_callback;
bool s_initialized = false;
bool s_in_progress = false;
bool s_fetch_pending = false;
WeatherData s_last_data;
esp_timer_handle_t s_timer = nullptr;
TaskHandle_t s_worker_task = nullptr;
char s_response_buf[12 * 1024] = {};
char s_decompressed_buf[12 * 1024] = {};
int s_response_len = 0;

bool DecodeGzipResponse() {
    const size_t input_len = static_cast<size_t>(s_response_len);
    const auto* input = reinterpret_cast<const uint8_t*>(s_response_buf);
    if (input_len < 2 || input[0] != 0x1f || input[1] != 0x8b) {
        return true;
    }
    if (input_len < 18 || input[2] != 8 || (input[3] & 0xe0) != 0) {
        ESP_LOGW(kTag, "Invalid GZip response header (len=%u)", static_cast<unsigned>(input_len));
        return false;
    }

    size_t offset = 10;
    const uint8_t flags = input[3];
    if (flags & 0x04) {
        if (offset + 2 > input_len - 8) return false;
        const size_t extra_len = input[offset] | (static_cast<size_t>(input[offset + 1]) << 8);
        offset += 2 + extra_len;
    }
    for (const uint8_t flag : {static_cast<uint8_t>(0x08), static_cast<uint8_t>(0x10)}) {
        if (flags & flag) {
            while (offset < input_len - 8 && input[offset] != 0) ++offset;
            if (offset >= input_len - 8) return false;
            ++offset;
        }
    }
    if (flags & 0x02) offset += 2;
    if (offset > input_len - 8) return false;

    const size_t compressed_len = input_len - offset - 8;
    const size_t decoded_len = tinfl_decompress_mem_to_mem(
        s_decompressed_buf, sizeof(s_decompressed_buf), input + offset, compressed_len,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (decoded_len == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
        ESP_LOGW(kTag, "Could not inflate GZip weather response (compressed=%u)",
                 static_cast<unsigned>(compressed_len));
        return false;
    }

    const size_t declared_size = input[input_len - 4] |
        (static_cast<size_t>(input[input_len - 3]) << 8) |
        (static_cast<size_t>(input[input_len - 2]) << 16) |
        (static_cast<size_t>(input[input_len - 1]) << 24);
    if (decoded_len != declared_size || decoded_len >= sizeof(s_response_buf)) {
        ESP_LOGW(kTag, "GZip weather response size mismatch (decoded=%u declared=%u)",
                 static_cast<unsigned>(decoded_len), static_cast<unsigned>(declared_size));
        return false;
    }

    memcpy(s_response_buf, s_decompressed_buf, decoded_len);
    s_response_len = static_cast<int>(decoded_len);
    s_response_buf[s_response_len] = '\0';
    ESP_LOGI(kTag, "Inflated GZip weather response: %u bytes", static_cast<unsigned>(decoded_len));
    return true;
}

const char* WeatherText(int code) {
    if (code == 0) return "晴";
    if (code <= 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code <= 57) return "毛毛雨";
    if (code <= 67) return "雨";
    if (code <= 77) return "雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "阵雪";
    if (code >= 95) return "雷雨";
    return "多云";
}

const char* WeatherIconCode(int code) {
    if (code == 0 || code == 1) return "100";
    if (code <= 3) return "101";
    if (code == 45 || code == 48) return "500";
    if (code <= 67 || (code >= 80 && code <= 82)) return "305";
    if (code <= 77 || (code >= 85 && code <= 86)) return "400";
    if (code >= 95) return "302";
    return "101";
}

std::string WindDirection(int degrees) {
    static constexpr const char* kDirections[] = {
        "北风", "东北风", "东风", "东南风", "南风", "西南风", "西风", "西北风"};
    const int index = ((degrees + 22) % 360) / 45;
    return kDirections[index];
}

int BeaufortScale(double speed_ms) {
    static constexpr double kUpperBounds[] = {
        0.3, 1.6, 3.4, 5.5, 8.0, 10.8, 13.9, 17.2, 20.8, 24.5, 28.5, 32.7};
    for (int scale = 0; scale < 12; ++scale) {
        if (speed_ms < kUpperBounds[scale]) return scale;
    }
    return 12;
}

bool ReadNumber(const cJSON* object, const char* key, double* value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) return false;
    *value = item->valuedouble;
    return true;
}

bool ValidApiHost(const char* host) {
    if (!host || !host[0] || strlen(host) >= sizeof(s_qweather_host)) return false;
    for (const char* p = host; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
    }
    return true;
}

bool ParseWeather(const char* json, WeatherData* data) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;

    const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON* daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    double value = 0;
    int code = 0;
    bool ok = current && daily && ReadNumber(current, "temperature_2m", &value);
    if (ok) {
        char text[16];
        snprintf(text, sizeof(text), "%.0f", value);
        data->temp = text;
        data->temp_int = static_cast<int>(value);
        if (ReadNumber(current, "apparent_temperature", &value)) {
            snprintf(text, sizeof(text), "%.0f", value);
            data->feels_like = text;
        }
        if (ReadNumber(current, "relative_humidity_2m", &value)) {
            snprintf(text, sizeof(text), "%.0f", value);
            data->humidity = text;
        }
        if (ReadNumber(current, "wind_speed_10m", &value)) {
            snprintf(text, sizeof(text), "%d", BeaufortScale(value));
            data->wind_scale = text;
        }
        if (ReadNumber(current, "wind_direction_10m", &value)) {
            data->wind_dir = WindDirection(static_cast<int>(value));
        }
        if (ReadNumber(current, "weather_code", &value)) code = static_cast<int>(value);
        data->weather_text = WeatherText(code);
        data->weather_icon = WeatherIconCode(code);

        const cJSON* time = cJSON_GetObjectItemCaseSensitive(current, "time");
        if (cJSON_IsString(time) && time->valuestring) data->update_time = time->valuestring;
        data->air_quality = "--";
        data->air_aqi = -1;

        const cJSON* times = cJSON_GetObjectItemCaseSensitive(daily, "time");
        const cJSON* codes = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
        const cJSON* highs = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
        const cJSON* lows = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
        const int count = cJSON_GetArraySize(times);
        for (int i = 0; i < count; ++i) {
            const cJSON* date = cJSON_GetArrayItem(times, i);
            const cJSON* day_code = cJSON_GetArrayItem(codes, i);
            const cJSON* high = cJSON_GetArrayItem(highs, i);
            const cJSON* low = cJSON_GetArrayItem(lows, i);
            if (!cJSON_IsString(date) || !cJSON_IsNumber(day_code) ||
                !cJSON_IsNumber(high) || !cJSON_IsNumber(low)) continue;

            WeatherForecastDay day;
            day.label = i == 0 ? "今天" : (i == 1 ? "明天" : date->valuestring + 5);
            day.weather_text = WeatherText(day_code->valueint);
            day.icon_code = WeatherIconCode(day_code->valueint);
            day.temp_max = static_cast<int32_t>(high->valuedouble);
            day.temp_min = static_cast<int32_t>(low->valuedouble);
            data->forecast.push_back(day);
        }
    }

    cJSON_Delete(root);
    return ok;
}

bool ParseQWeatherCurrent(const char* json, WeatherData* data) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON* condition = cJSON_GetObjectItemCaseSensitive(root, "condition");
    const cJSON* temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    const cJSON* feels_like = cJSON_GetObjectItemCaseSensitive(root, "feelsLike");
    const cJSON* wind = cJSON_GetObjectItemCaseSensitive(root, "wind");
    const cJSON* humidity = cJSON_GetObjectItemCaseSensitive(root, "humidity");
    double value = 0;
    bool ok = condition && temperature &&
              ReadNumber(temperature, "value", &value);
    if (ok) {
        char text[16];
        snprintf(text, sizeof(text), "%.0f", value);
        data->temp = text;
        data->temp_int = static_cast<int>(value);
        if (ReadNumber(feels_like, "value", &value)) {
            snprintf(text, sizeof(text), "%.0f", value);
            data->feels_like = text;
        }
        if (cJSON_IsNumber(humidity)) {
            snprintf(text, sizeof(text), "%.0f", humidity->valuedouble * 100.0);
            data->humidity = text;
        }
        const cJSON* text_item = cJSON_GetObjectItemCaseSensitive(condition, "text");
        const cJSON* code_item = cJSON_GetObjectItemCaseSensitive(condition, "code");
        if (cJSON_IsString(text_item)) data->weather_text = text_item->valuestring;
        if (cJSON_IsString(code_item)) data->weather_icon = code_item->valuestring;
        const cJSON* wind_direction = cJSON_GetObjectItemCaseSensitive(wind, "direction");
        const cJSON* wind_compass = cJSON_GetObjectItemCaseSensitive(wind_direction, "compass");
        const cJSON* wind_scale = cJSON_GetObjectItemCaseSensitive(wind, "scale");
        if (cJSON_IsNumber(wind_scale)) {
            snprintf(text, sizeof(text), "%d", wind_scale->valueint);
            data->wind_scale = text;
        }
        if (cJSON_IsString(wind_compass)) {
            static const char* kCompass[] = {"n", "nne", "ne", "ene", "e", "ese", "se", "sse",
                                             "s", "ssw", "sw", "wsw", "w", "wnw", "nw", "nnw"};
            static const char* kChinese[] = {"北风", "北东北风", "东北风", "东东北风", "东风", "东东南风", "东南风", "南东南风",
                                             "南风", "南西南风", "西南风", "西西南风", "西风", "西西北风", "西北风", "北西北风"};
            for (int i = 0; i < 16; ++i) {
                if (strcmp(wind_compass->valuestring, kCompass[i]) == 0) {
                    data->wind_dir = kChinese[i];
                    break;
                }
            }
        }
        data->air_quality = "--";
        data->air_aqi = -1;
    }
    cJSON_Delete(root);
    return ok;
}

void ParseQWeatherAirQuality(const char* json, WeatherData* data) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return;

    const cJSON* indexes = cJSON_GetObjectItemCaseSensitive(root, "indexes");
    const cJSON* selected = nullptr;
    const int count = cJSON_GetArraySize(indexes);
    for (int i = 0; i < count; ++i) {
        const cJSON* item = cJSON_GetArrayItem(indexes, i);
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(item, "code");
        if (cJSON_IsString(code) && strcmp(code->valuestring, "cn-mee") == 0) {
            selected = item;
            break;
        }
        if (!selected && cJSON_IsObject(item)) selected = item;
    }

    if (selected) {
        const cJSON* aqi = cJSON_GetObjectItemCaseSensitive(selected, "aqi");
        const cJSON* category = cJSON_GetObjectItemCaseSensitive(selected, "category");
        if (cJSON_IsNumber(aqi)) data->air_aqi = static_cast<int32_t>(aqi->valuedouble + 0.5);
        const cJSON* display = cJSON_GetObjectItemCaseSensitive(selected, "aqiDisplay");
        if (cJSON_IsString(display) && display->valuestring && display->valuestring[0]) {
            data->air_aqi = atoi(display->valuestring);
        }
        if (cJSON_IsString(category) && category->valuestring) data->air_quality = category->valuestring;
    }
    cJSON_Delete(root);
}

bool ParseQWeatherDaily(const char* json, WeatherData* data) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    const cJSON* days = cJSON_GetObjectItemCaseSensitive(root, "days");
    const int count = cJSON_GetArraySize(days);
    data->forecast.clear();
    for (int i = 0; i < count && i < 4; ++i) {
        const cJSON* item = cJSON_GetArrayItem(days, i);
        const cJSON* daytime = cJSON_GetObjectItemCaseSensitive(item, "daytime");
        const cJSON* condition = cJSON_GetObjectItemCaseSensitive(daytime, "condition");
        const cJSON* high = cJSON_GetObjectItemCaseSensitive(daytime, "temperatureMax");
        const cJSON* low = cJSON_GetObjectItemCaseSensitive(daytime, "temperatureMin");
        double high_value = 0, low_value = 0;
        if (!ReadNumber(high, "value", &high_value) || !ReadNumber(low, "value", &low_value)) continue;
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(condition, "text");
        const cJSON* code = cJSON_GetObjectItemCaseSensitive(condition, "code");
        WeatherForecastDay day;
        day.label = i == 0 ? "今天" : (i == 1 ? "明天" : "后天");
        if (cJSON_IsString(text)) day.weather_text = text->valuestring;
        if (cJSON_IsString(code)) day.icon_code = code->valuestring;
        day.temp_max = static_cast<int32_t>(high_value);
        day.temp_min = static_cast<int32_t>(low_value);
        data->forecast.push_back(day);
    }
    cJSON_Delete(root);
    return !data->forecast.empty();
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_DATA &&
        s_response_len + event->data_len < sizeof(s_response_buf)) {
        memcpy(s_response_buf + s_response_len, event->data, event->data_len);
        s_response_len += event->data_len;
    }
    return ESP_OK;
}

bool HttpGet(const char* url, const char* credential = nullptr) {
    s_response_len = 0;
    memset(s_response_buf, 0, sizeof(s_response_buf));
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_GET;
    config.event_handler = HttpEventHandler;
    config.timeout_ms = 10000;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    // QWeather compresses API responses by default. The ESP-IDF HTTP client
    // does not transparently inflate the collected event data, so explicitly
    // request an identity (uncompressed) response before passing it to cJSON.
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    if (credential && credential[0]) {
        esp_http_client_set_header(client, "X-QW-Api-Key", credential);
    }
    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK || status != 200) {
        ESP_LOGW(kTag, "Forecast request failed: %s, HTTP %d",
                 esp_err_to_name(result), status);
        return false;
    }
    s_response_buf[s_response_len] = '\0';
    return DecodeGzipResponse();
}

bool DoFetch() {
    char city_name[sizeof(s_city_name)];
    strncpy(city_name, s_city_name, sizeof(city_name) - 1);
    city_name[sizeof(city_name) - 1] = '\0';
    char provider[sizeof(s_provider)];
    char qweather_host[sizeof(s_qweather_host)];
    char qweather_credential[sizeof(s_qweather_credential)];
    strlcpy(provider, s_provider, sizeof(provider));
    strlcpy(qweather_host, s_qweather_host, sizeof(qweather_host));
    strlcpy(qweather_credential, s_qweather_credential, sizeof(qweather_credential));
    const double latitude = s_latitude;
    const double longitude = s_longitude;
    WeatherData data;
    data.city = city_name;
    if (strcmp(provider, "qweather") == 0) {
        if (!qweather_host[0] || !qweather_credential[0]) {
            ESP_LOGW(kTag, "QWeather selected but Host or credential is missing");
            return false;
        }
        char url[384];
        snprintf(url, sizeof(url), "https://%s/weather/v1/current/%.2f/%.2f?lang=zh",
                 qweather_host, latitude, longitude);
        if (!HttpGet(url, qweather_credential)) {
            ESP_LOGW(kTag, "Could not load QWeather current conditions");
            return false;
        }
        if (!ParseQWeatherCurrent(s_response_buf, &data)) {
            ESP_LOGW(kTag, "QWeather current JSON not recognized (len=%d, prefix=%02x %02x %02x %02x)",
                     s_response_len, static_cast<uint8_t>(s_response_buf[0]),
                     static_cast<uint8_t>(s_response_buf[1]),
                     static_cast<uint8_t>(s_response_buf[2]),
                     static_cast<uint8_t>(s_response_buf[3]));
            return false;
        }
        snprintf(url, sizeof(url), "https://%s/weather/v1/daily/%.2f/%.2f?days=4&localTime=true&lang=zh",
                 qweather_host, latitude, longitude);
        if (!HttpGet(url, qweather_credential) ||
            !ParseQWeatherDaily(s_response_buf, &data)) {
            ESP_LOGW(kTag, "Could not load QWeather daily forecast");
        }
        snprintf(url, sizeof(url), "https://%s/airquality/v1/current/%.2f/%.2f?lang=zh",
                 qweather_host, latitude, longitude);
        if (HttpGet(url, qweather_credential)) {
            ParseQWeatherAirQuality(s_response_buf, &data);
            ESP_LOGI(kTag, "QWeather air quality: AQI=%d, %s",
                     data.air_aqi, data.air_quality.c_str());
        } else {
            ESP_LOGW(kTag, "Could not load QWeather air quality");
        }
        data.source = "QWeather · developer.qweather.com";
    } else {
        char url[512];
        snprintf(url, sizeof(url), kForecastUrl, latitude, longitude);
        if (!HttpGet(url) || !ParseWeather(s_response_buf, &data)) {
            ESP_LOGW(kTag, "Could not load Open-Meteo forecast");
            return false;
        }
        data.source = "Open-Meteo.com · CC BY 4.0";
    }

    s_last_data = data;
    if (s_callback) s_callback(s_last_data);
    ESP_LOGI(kTag, "Weather updated for %s: %s C, %s",
             city_name, data.temp.c_str(), data.weather_text.c_str());
    return true;
}

void ScheduleFetch() {
    if (!s_initialized || !s_timer || !s_worker_task) return;
    if (s_in_progress) {
        s_fetch_pending = true;
        return;
    }
    esp_timer_stop(s_timer);
    const esp_err_t result = esp_timer_start_once(s_timer, 1000);
    if (result != ESP_OK) ESP_LOGW(kTag, "Could not schedule weather fetch: %s", esp_err_to_name(result));
}

void WorkerTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_in_progress = true;
        const bool success = DoFetch();
        s_in_progress = false;

        if (s_fetch_pending) {
            s_fetch_pending = false;
            ScheduleFetch();
        } else {
            const int64_t delay = success ? kRefreshIntervalUs : kRetryIntervalUs;
            const esp_err_t result = esp_timer_start_once(s_timer, delay);
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "Could not schedule weather retry: %s", esp_err_to_name(result));
            }
        }
    }
}

void TimerCallback(void*) {
    if (s_worker_task) xTaskNotifyGive(s_worker_task);
}

}  // namespace

WeatherIcon ParseWeatherIcon(const char* text) {
    if (!text || !text[0]) return WeatherIcon::Unknown;
    if (strstr(text, "晴")) return WeatherIcon::Sunny;
    if (strstr(text, "多云")) return WeatherIcon::Cloudy;
    if (strstr(text, "阴")) return WeatherIcon::Overcast;
    if (strstr(text, "雨")) return WeatherIcon::Rain;
    if (strstr(text, "雪")) return WeatherIcon::Snow;
    if (strstr(text, "雾") || strstr(text, "霾")) return WeatherIcon::Fog;
    return WeatherIcon::Unknown;
}

void weather_api_init(const char* city_name, double latitude, double longitude,
                      WeatherCallback callback) {
    if (s_initialized) return;
    strncpy(s_city_name, city_name ? city_name : "", sizeof(s_city_name) - 1);
    s_latitude = latitude;
    s_longitude = longitude;
    s_callback = std::move(callback);
    Settings config("weather_api", true);
    strlcpy(s_provider, config.GetString("provider", "open-meteo").c_str(), sizeof(s_provider));
    strlcpy(s_qweather_host, config.GetString("qw_host", "").c_str(), sizeof(s_qweather_host));
    const std::string saved_auth = config.GetString("qw_auth", "api_key");
    strlcpy(s_qweather_credential, config.GetString("qw_credential", "").c_str(), sizeof(s_qweather_credential));
    if (saved_auth == "jwt") {
        // A stored bearer JWT must never be sent as an API key after simplifying this build.
        s_qweather_credential[0] = '\0';
        config.EraseKey("qw_credential");
    }
    config.SetString("qw_auth", "api_key");

    if (xTaskCreate(WorkerTask, "weather_fetch", 16 * 1024, nullptr, 3,
                    &s_worker_task) != pdPASS) {
        ESP_LOGE(kTag, "Could not create weather worker task");
        return;
    }

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = TimerCallback;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "weather_refresh";
    if (esp_timer_create(&timer_args, &s_timer) != ESP_OK) {
        ESP_LOGE(kTag, "Could not create refresh timer");
        return;
    }
    s_initialized = true;
    ScheduleFetch();
}

bool weather_api_fetch_now() {
    if (!s_initialized) return false;
    ScheduleFetch();
    return true;
}

void weather_api_set_location(const char* city_name, double latitude, double longitude) {
    strncpy(s_city_name, city_name ? city_name : "", sizeof(s_city_name) - 1);
    s_city_name[sizeof(s_city_name) - 1] = '\0';
    s_latitude = latitude;
    s_longitude = longitude;
    ScheduleFetch();
}

const char* weather_api_get_city() {
    return s_city_name;
}

bool weather_api_is_ready() {
    return s_initialized;
}

const WeatherData* weather_api_get_last_data() {
    return &s_last_data;
}

bool weather_api_set_provider(const char* provider, const char* api_host,
                              const char* credential) {
    if (!provider || (strcmp(provider, "open-meteo") != 0 && strcmp(provider, "qweather") != 0)) return false;
    const char* next_host = api_host && api_host[0] ? api_host : s_qweather_host;
    const char* next_credential = credential && credential[0] ? credential : s_qweather_credential;
    if (api_host && api_host[0] && !ValidApiHost(api_host)) return false;
    if (credential && strlen(credential) >= sizeof(s_qweather_credential)) return false;
    if (strcmp(provider, "qweather") == 0 && (!ValidApiHost(next_host) || !next_credential[0])) return false;
    Settings config("weather_api", true);
    s_provider[0] = '\0';
    strlcpy(s_provider, provider, sizeof(s_provider));
    config.SetString("provider", s_provider);
    if (api_host && api_host[0]) {
        strlcpy(s_qweather_host, api_host, sizeof(s_qweather_host));
        config.SetString("qw_host", s_qweather_host);
    }
    if (credential && credential[0]) {
        strlcpy(s_qweather_credential, credential, sizeof(s_qweather_credential));
        config.SetString("qw_credential", s_qweather_credential);
    }
    if (s_initialized) ScheduleFetch();
    return true;
}

void weather_api_get_provider(char* provider, size_t provider_size,
                              char* api_host, size_t api_host_size,
                              bool* credential_configured) {
    if (provider && provider_size) strlcpy(provider, s_provider, provider_size);
    if (api_host && api_host_size) strlcpy(api_host, s_qweather_host, api_host_size);
    if (credential_configured) *credential_configured = s_qweather_credential[0] != '\0';
}
