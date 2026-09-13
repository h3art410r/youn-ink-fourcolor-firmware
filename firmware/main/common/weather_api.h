/**
 * @file weather_api.h
 * @brief Open-Meteo weather client for ESP32
 *
 * Fetches current conditions and a short forecast by geographic coordinates.
 *
 * The public forecast endpoint does not require an API key for personal use.
 */

#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string>
#include <functional>
#include <vector>

// ============================================================
// Weather data model
// ============================================================

/**
 * @brief Normalized weather data for the UI
 */
struct WeatherForecastDay {
    std::string label;        // Today / Tomorrow / etc.
    std::string weather_text; // 晴 / 多云 / 小雨
    std::string icon_code;    // Renderer-compatible weather icon code
    int32_t temp_min = 0;
    int32_t temp_max = 0;
};

struct WeatherData {
    std::string city;         // City name in Chinese
    std::string source;        // Provider name for on-screen attribution
    std::string temp;         // Current temperature (e.g., "25")
    std::string feels_like;   // Feels like temperature (e.g., "27")
    std::string weather_icon; // Renderer-compatible icon code (e.g., "100")
    std::string weather_text; // Weather condition (e.g., "晴", "多云", "小雨")
    std::string wind_dir;     // Wind direction (e.g., "东南风")
    std::string wind_scale;   // Wind scale (e.g., "3")
    std::string humidity;     // Humidity percentage (e.g., "45")
    std::string update_time;  // Last update time
    std::string air_quality;  // Air-quality label, empty when unavailable
    int32_t air_aqi = -1;     // AQI number
    std::string dress_advice; // Dressing advice from life indices, empty when unavailable
    int32_t temp_int;         // Numeric temperature for icon selection
    std::vector<WeatherForecastDay> forecast;
};

/**
 * @brief Weather icon codes for 1bpp rendering
 * Maps weather condition text to icon character codes.
 */
enum class WeatherIcon {
    Sunny,       // 晴
    Cloudy,      // 多云
    Overcast,    // 阴
    Rain,        // 雨 (any rain type)
    Snow,        // 雪
    Fog,         // 雾
    Unknown,     // Fallback
};

/**
 * @brief Map weather condition text to icon type
 */
WeatherIcon ParseWeatherIcon(const char* weather_text);

// ============================================================
// API interface
// ============================================================

/**
 * @brief Callback type for weather data delivery
 */
using WeatherCallback = std::function<void(const WeatherData&)>;

/**
 * @brief Initialize the Open-Meteo client and fetch the first city.
 */
void weather_api_init(const char* city_name, double latitude, double longitude,
                      WeatherCallback callback);

/**
 * @brief Trigger a manual weather data fetch
 *
 * @return true if a request was queued
 */
bool weather_api_fetch_now();

/**
 * @brief Change city and queue a fetch without blocking button handling.
 */
void weather_api_set_location(const char* city_name, double latitude, double longitude);

/**
 * @brief Get the current city name
 */
const char* weather_api_get_city();

/**
 * @brief Check if API client is initialized
 */
bool weather_api_is_ready();

/**
 * @brief Get the last fetched weather data
 */
const WeatherData* weather_api_get_last_data();

// Configure the active provider. Supported values: "open-meteo", "qweather".
// QWeather credentials are stored per-device and are never returned by GET APIs.
bool weather_api_set_provider(const char* provider, const char* api_host,
                              const char* credential);
void weather_api_get_provider(char* provider, size_t provider_size,
                              char* api_host, size_t api_host_size,
                              bool* credential_configured);

#endif  // WEATHER_API_H
