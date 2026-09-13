/**
 * @file weather_renderer.cc
 * @brief Rawdraw weather page renderer for 400x300 EPD
 */

#include "weather_renderer.h"

#include "common/weather_api.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t weather_icons_48;
extern const lv_font_t weather_icons_16;

namespace rawdraw {

namespace {

struct ForecastRenderItem {
    std::string label;
    std::string weather_text;
    std::string icon_code;
    int32_t temp_min = 0;
    int32_t temp_max = 0;
};

std::string FitTextToWidth(const std::string& text, const lv_font_t* font, int max_width) {
    if (!font || max_width <= 0 || text.empty()) return "";
    if (MeasureTextWidth(text.c_str(), font) <= max_width) return text;

    std::string out;
    const char* p = text.c_str();
    while (*p) {
        const char* start = p;
        utf8_next(&p);
        std::string next = out;
        next.append(start, p - start);
        if (MeasureTextWidth((next + "...").c_str(), font) > max_width) break;
        out = std::move(next);
    }
    return out + "...";
}

const char* IconGlyphForCode(const std::string& icon_code, const std::string& weather_text) {
    auto starts_with_digit = [](const std::string& value, int low, int high) -> bool {
        if (value.empty()) return false;
        int code = atoi(value.c_str());
        return code >= low && code <= high;
    };

    if (starts_with_digit(icon_code, 100, 100) || weather_text.find("晴") != std::string::npos) {
        return "\xef\x83\x9e";  // sun
    }
    if (starts_with_digit(icon_code, 101, 103) || weather_text.find("多云") != std::string::npos ||
        weather_text.find("晴间多云") != std::string::npos) {
        return "\xef\x83\x82";  // cloud-sun-ish fallback
    }
    if (starts_with_digit(icon_code, 104, 104) || weather_text.find("阴") != std::string::npos) {
        return "\xef\x83\x82";  // cloud
    }
    if (starts_with_digit(icon_code, 300, 399) || weather_text.find("雨") != std::string::npos) {
        return "\xef\x83\xa9";  // rain
    }
    if (starts_with_digit(icon_code, 400, 499) || weather_text.find("雪") != std::string::npos) {
        return "\xef\x8b\x9c";  // snow
    }
    if (starts_with_digit(icon_code, 500, 599) || weather_text.find("雾") != std::string::npos ||
        weather_text.find("霾") != std::string::npos) {
        return "\xef\x9d\x9f";  // smog/fog
    }
    return "\xef\x83\x9e";      // default sun
}

[[maybe_unused]] void DrawForecastCard(uint8_t* fb,
                                       int width,
                                       const Rect& r,
                                       const ForecastRenderItem& day,
                                       bool selected,
                                       const lv_font_t* font,
                                       const lv_font_t* icon_font) {
    DrawRect(fb, width, r, WHITE);
    DrawRoundRect(fb, width, r, Style::kBorderRadiusSM, WHITE, BLACK,
                  selected ? Style::kBorderMedium : Style::kBorderThin);
    if (selected) {
        DrawRect(fb, width, {r.x + r.w - 15, r.y + 7, 8, 2}, BLACK);
    }

    const int label_baseline = CalcBaselineY(font, r.y + 13, Style::kVisualTextOffset);
    const int label_y = TopYFromBaseline(font, label_baseline);
    DrawText(fb, width, r.x + 8, label_y, day.label.c_str(), font, BLACK);

    const char* glyph = IconGlyphForCode(day.icon_code, day.weather_text);
    const int icon_circle_r = 10;
    const int icon_cx = r.x + r.w / 2;
    const int icon_cy = r.y + 30;
    DrawCircleBorder(fb, width, {icon_cx, icon_cy}, icon_circle_r, 1, BLACK);
    int icon_w = MeasureTextWidth(glyph, icon_font);
    DrawIcon(fb, width, icon_cx - icon_w / 2, icon_cy - 8, glyph, icon_font, BLACK);

    char temp_buf[24];
    snprintf(temp_buf, sizeof(temp_buf), "%d~%d°", static_cast<int>(day.temp_min), static_cast<int>(day.temp_max));
    int temp_w = MeasureTextWidth(temp_buf, font);
    const int temp_baseline = CalcBaselineY(font, r.y + r.h - 12, Style::kVisualTextOffset);
    DrawText(fb, width, r.x + (r.w - temp_w) / 2, TopYFromBaseline(font, temp_baseline),
             temp_buf, font, BLACK);
}

std::vector<ForecastRenderItem> BuildForecastItems(const WeatherData& data) {
    std::vector<ForecastRenderItem> items;
    items.reserve(4);

    for (size_t i = 0; i < data.forecast.size() && items.size() < 4; ++i) {
        const auto& src = data.forecast[i];
        items.push_back({src.label, src.weather_text, src.icon_code, src.temp_min, src.temp_max});
    }

    return items;
}

}  // namespace

WeatherRenderer::WeatherRenderer()
    : has_data_(false)
    , page_index_(0)
    , font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim) {
}

WeatherRenderer::~WeatherRenderer() {}

void WeatherRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    has_data_ = false;
    needs_full_refresh_ = true;
    page_index_ = 0;
    firmware_version_.clear();
}

void WeatherRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle bg_style = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle card_style = theme.Component(ComponentRole::CardDefault);
    const PaintStyle panel_style = theme.Component(ComponentRole::Panel);
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);
    const Color accent = theme.ColorFor(ThemeToken::Accent);

    const int content_top = Style::kStatusBarHeight + 2;
    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, bg_style);

    if (!has_data_) {
        const char* empty_text = "暂无天气数据";
        const char* hint = "联网后自动获取";
        int text_w = MeasureTextWidth(empty_text, font_);
        int hint_w = MeasureTextWidth(hint, font_);
        int center_y = content_top + (height - content_top) / 2;
        const std::string location = city_name_.empty() ? "天气" : city_name_;
        const int location_w = MeasureTextWidth(location.c_str(), title_font_);
        const int location_baseline = CalcBaselineY(title_font_, center_y - 34, Style::kVisualTextOffset);
        const int empty_baseline = CalcBaselineY(font_, center_y, Style::kVisualTextOffset);
        const int hint_baseline = CalcBaselineY(font_, center_y + 25, Style::kVisualTextOffset);
        DrawText(fb, width, (width - location_w) / 2,
                 TopYFromBaseline(title_font_, location_baseline), location.c_str(), title_font_, text);
        DrawText(fb, width, (width - text_w) / 2, TopYFromBaseline(font_, empty_baseline), empty_text, font_, text);
        DrawText(fb, width, (width - hint_w) / 2, TopYFromBaseline(font_, hint_baseline), hint, font_, secondary);
    } else {
        std::string location = city_name_.empty() ? current_data_.city : city_name_;
        if (location.empty()) location = "杭州";
        const std::string location_line = FitTextToWidth(location, title_font_, 220);

        // A simple, consistent reading order: city/source, current weather,
        // air and comfort metrics, then the short forecast.
        const int header_y = Style::kStatusBarHeight + 3;
        DrawText(fb, width, 18,
                 InkCenteredTextTopY(title_font_, location_line.c_str(), header_y + 4, 0),
                 location_line.c_str(), title_font_, text);
        const std::string source = current_data_.source.find("QWeather") != std::string::npos
            ? "和风天气"
            : (current_data_.source.empty() ? "天气数据" : "Open-Meteo · CC BY 4.0");
        const int source_w = MeasureTextWidth(source.c_str(), font_);
        DrawText(fb, width, width - source_w - 18,
                 InkCenteredTextTopY(font_, source.c_str(), header_y + 7, 0),
                 source.c_str(), font_, secondary);

        Rect hero{16, 57, width - 32, 84};
        DrawStyledRoundRect(fb, width, height, hero, Style::kBorderRadiusLG, card_style);
        char temp_buf[20];
        snprintf(temp_buf, sizeof(temp_buf), "%s°", current_data_.temp.empty() ? "--" : current_data_.temp.c_str());
        DrawText(fb, width, 34,
                 InkCenteredTextTopY(title_font_, temp_buf, hero.y + 13, 0),
                 temp_buf, title_font_, text);
        char feels_buf[32];
        snprintf(feels_buf, sizeof(feels_buf), "体感 %s°",
                 current_data_.feels_like.empty() ? (current_data_.temp.empty() ? "--" : current_data_.temp.c_str()) : current_data_.feels_like.c_str());
        DrawText(fb, width, 36,
                 InkCenteredTextTopY(font_, feels_buf, hero.y + 55, 0),
                 feels_buf, font_, secondary);

        const char* desc_glyph = IconGlyphForCode(current_data_.weather_icon, current_data_.weather_text);
        const int icon_w = MeasureTextWidth(desc_glyph, &weather_icons_48);
        DrawIcon(fb, width, 258 - icon_w / 2,
                 InkCenteredTextTopY(&weather_icons_48, desc_glyph, hero.y + 8, 0),
                 desc_glyph, &weather_icons_48, accent);
        const std::string desc = FitTextToWidth(current_data_.weather_text.empty() ? "天气 --" : current_data_.weather_text,
                                                title_font_, 150);
        const int desc_w = MeasureTextWidth(desc.c_str(), title_font_);
        DrawText(fb, width, 258 - desc_w / 2,
                 InkCenteredTextTopY(title_font_, desc.c_str(), hero.y + 57, 0),
                 desc.c_str(), title_font_, text);

        Rect metrics{16, 150, width - 32, 48};
        DrawStyledRoundRect(fb, width, height, metrics, Style::kBorderRadiusMD, panel_style);
        const int cell_w = metrics.w / 3;
        const char* labels[] = {"空气质量", "湿度", "风力 / 风向"};
        std::string values[] = {
            current_data_.air_aqi >= 0
                ? std::to_string(current_data_.air_aqi) + " " + (current_data_.air_quality.empty() ? "" : current_data_.air_quality)
                : (current_data_.air_quality.empty() ? "暂无数据" : current_data_.air_quality),
            current_data_.humidity.empty() ? "--%" : current_data_.humidity + "%",
            (current_data_.wind_scale.empty() ? "--" : current_data_.wind_scale + "级") +
                (current_data_.wind_dir.empty() ? "" : " " + current_data_.wind_dir),
        };
        for (int i = 0; i < 3; ++i) {
            const int x = metrics.x + i * cell_w;
            if (i > 0) DrawVLine(fb, width, x, metrics.y + 8, metrics.y + metrics.h - 8, border);
            const int label_w = MeasureTextWidth(labels[i], font_);
            const int value_w = MeasureTextWidth(values[i].c_str(), font_);
            DrawText(fb, width, x + (cell_w - label_w) / 2,
                     InkCenteredTextTopY(font_, labels[i], metrics.y + 7, 0), labels[i], font_, secondary);
            DrawText(fb, width, x + (cell_w - value_w) / 2,
                     InkCenteredTextTopY(font_, values[i].c_str(), metrics.y + 28, 0), values[i].c_str(), font_, text);
        }

        const std::vector<ForecastRenderItem> forecast_items = BuildForecastItems(current_data_);
        const int forecast_count = static_cast<int>(forecast_items.size());
        if (page_index_ >= forecast_count && forecast_count > 0) {
            page_index_ = forecast_count - 1;
        }

        Rect forecast_panel{16, 210, width - 32, 66};
        DrawStyledRoundRect(fb, width, height, forecast_panel, Style::kBorderRadiusMD, panel_style);
        const int card_w = forecast_panel.w / 4;
        for (int i = 0; i < forecast_count && i < 4; ++i) {
            const auto& item = forecast_items[i];
            const int x = forecast_panel.x + i * card_w;
            if (i > 0) {
                for (int y = forecast_panel.y + 8; y < forecast_panel.y + forecast_panel.h - 8; y += 4) {
                    set_pixel(fb, width, x, y, border);
                }
            }
            const int label_w = MeasureTextWidth(item.label.c_str(), font_);
            DrawText(fb, width, x + (card_w - label_w) / 2,
                     InkCenteredTextTopY(font_, item.label.c_str(), forecast_panel.y + 12, 0),
                     item.label.c_str(), font_, secondary);
            const char* glyph = IconGlyphForCode(item.icon_code, item.weather_text);
            const int icon_center_y = forecast_panel.y + 30;
            const int forecast_icon_w = MeasureTextWidth(glyph, &weather_icons_16);
            DrawIcon(fb, width, x + (card_w - forecast_icon_w) / 2, InkCenteredTextTopY(&weather_icons_16, glyph, icon_center_y, 0),
                     glyph, &weather_icons_16, accent);
            char temp_range[24];
            snprintf(temp_range, sizeof(temp_range), "%d/%d°C", static_cast<int>(item.temp_min), static_cast<int>(item.temp_max));
            const int range_w = MeasureTextWidth(temp_range, font_);
            DrawText(fb, width, x + (card_w - range_w) / 2,
                     InkCenteredTextTopY(font_, temp_range, forecast_panel.y + 48, 0),
                     temp_range, font_, text);
        }
    }

    needs_full_refresh_ = false;
}

bool WeatherRenderer::HandleInput(const ButtonEvent& event) {
    const int max_cards = std::min<int>(4, static_cast<int>(BuildForecastItems(current_data_).size()));
    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (max_cards > 0) {
                page_index_ = std::max(0, page_index_ - 1);
                needs_full_refresh_ = true;
                return true;
            }
            return false;
        case ButtonEvent::kDownClick:
            if (max_cards > 0) {
                page_index_ = std::min(max_cards - 1, page_index_ + 1);
                needs_full_refresh_ = true;
                return true;
            }
            return false;
        case ButtonEvent::kUpLongPress:
        case ButtonEvent::kDownLongPress:
        case ButtonEvent::kBootLongPress:
            weather_api_fetch_now();
            needs_full_refresh_ = true;
            return true;
        default:
            return false;
    }
}

void WeatherRenderer::Update(const WeatherData& data) {
    current_data_ = data;
    has_data_ = true;
    const int max_cards = std::min<int>(4, static_cast<int>(BuildForecastItems(current_data_).size()));
    if (page_index_ >= max_cards) {
        page_index_ = max_cards > 0 ? max_cards - 1 : 0;
    }
    needs_full_refresh_ = true;
}

void WeatherRenderer::SetCityName(const char* name) {
    const std::string next_name = name ? name : "";
    if (city_name_ != next_name) {
        city_name_ = next_name;
        has_data_ = false;
    }
    needs_full_refresh_ = true;
}

void WeatherRenderer::SetFirmwareVersion(const char* version) {
    firmware_version_ = version ? version : "";
    needs_full_refresh_ = true;
}

}  // namespace rawdraw
