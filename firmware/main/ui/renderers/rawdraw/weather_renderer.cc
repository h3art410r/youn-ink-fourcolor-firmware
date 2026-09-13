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
#include <cstdlib>
#include <cstring>
#include <vector>

// External font references
extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t SourceHanSansSC_12;
extern const lv_font_t TempDigits72;
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

void DrawCenteredTextInBox(uint8_t* fb, int width, const Rect& box,
                           const std::string& text, const lv_font_t* font, Color color) {
    if (text.empty() || !font) return;
    const int text_width = MeasureTextWidth(text.c_str(), font);
    const int x = box.x + std::max(0, (box.w - text_width) / 2);
    const int y = InkCenteredTextTopYInBox(font, text.c_str(), box.y, box.h, 0);
    DrawText(fb, width, x, y, text.c_str(), font, color);
}

const char* IconGlyphForCode(const std::string& icon_code, const std::string& weather_text) {
    auto starts_with_digit = [](const std::string& value, int low, int high) -> bool {
        if (value.empty()) return false;
        int code = atoi(value.c_str());
        return code >= low && code <= high;
    };

    if (starts_with_digit(icon_code, 100, 100) || weather_text.find("晴") != std::string::npos) {
        return "\xef\x86\x85";  // Font Awesome sun (U+F185)
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
        return "\xef\x83\x82";  // cloud fallback; the compact weather font has no fog glyph
    }
    return "\xef\x86\x85";      // default sun (U+F185)
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
    DrawIcon(fb, width, icon_cx - icon_w / 2, icon_cy - 8, glyph, icon_font, RED);

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
    const Color text = theme.ColorFor(ThemeToken::TextPrimary);
    const Color secondary = theme.ColorFor(ThemeToken::TextSecondary);
    const Color border = theme.ColorFor(ThemeToken::Border);

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
        const std::string location_line = FitTextToWidth(location, title_font_, 150);

        // Three-zone weather layout: header 32-54 (city left, attribution
        // Two-column hero: a giant 72px temperature dominates the left
        // column, a hairline divider at x196 declares the split, and the
        // right column clusters the icon with feels-like and today's range.
        // Fact caption 136-154 centered, dressing advice vertically
        // centered in its 160-192 zone, divider at 200, four forecast
        // columns 206-282 separated by dashed verticals.
        DrawText(fb, width, 16,
                 InkCenteredTextTopYInBox(title_font_, location_line.c_str(), 32, 22, 0),
                 location_line.c_str(), title_font_, text);
        const std::string attribution = current_data_.source.find("QWeather") != std::string::npos
            ? "和风天气"
            : "Open-Meteo";
        const int attribution_w = MeasureTextWidth(attribution.c_str(), font_);
        DrawText(fb, width, width - 16 - attribution_w,
                 InkCenteredTextTopYInBox(font_, attribution.c_str(), 32, 22, 0),
                 attribution.c_str(), font_, secondary);

        // Left column: giant digits + raised degree sign + condition, with
        // the condition baseline-aligned to the bottom of the digits.
        const std::string temp_str = current_data_.temp.empty() ? "--" : current_data_.temp;
        const std::string cond_str = FitTextToWidth(
            current_data_.weather_text.empty() ? "--" : current_data_.weather_text,
            title_font_, 72);
        const lv_font_t* digits_font = &TempDigits72;
        const int hero_center = 88;
        DrawText(fb, width, 16,
                 InkCenteredTextTopY(digits_font, temp_str.c_str(), hero_center, 0),
                 temp_str.c_str(), digits_font, text);
        const int digits_w = MeasureTextWidth(temp_str.c_str(), digits_font);
        const int digits_ink = MeasureTextInkBounds(digits_font, temp_str.c_str()).height;
        const int digits_bottom = hero_center + digits_ink / 2;
        const int deg_x = 16 + digits_w + 4;
        DrawText(fb, width, deg_x,
                 InkCenteredTextTopY(title_font_, "°", hero_center - digits_ink / 2 + 8, 0),
                 "°", title_font_, text);
        const int cond_x = deg_x + MeasureTextWidth("°", title_font_) + 10;
        DrawText(fb, width, cond_x,
                 InkCenteredTextTopY(title_font_, cond_str.c_str(), digits_bottom - 12, 0),
                 cond_str.c_str(), title_font_, text);

        // Hairline divider splitting the hero into two columns.
        DrawVLine(fb, width, 196, 64, 116, border);

        // Right column: icon at the right edge, feels-like and today's
        // range stacked to its left.
        const char* desc_glyph = IconGlyphForCode(current_data_.weather_icon, current_data_.weather_text);
        const int icon_w = MeasureTextWidth(desc_glyph, &weather_icons_48);
        const int icon_x = width - 16 - icon_w;
        DrawIcon(fb, width, icon_x,
                 InkCenteredTextTopY(&weather_icons_48, desc_glyph, hero_center, 0),
                 desc_glyph, &weather_icons_48, RED);
        const std::string feels_str = current_data_.feels_like.empty() ? temp_str : current_data_.feels_like;
        const std::string feels_line = "体感 " + feels_str + "°";
        char range_buf[24];
        if (!current_data_.forecast.empty()) {
            snprintf(range_buf, sizeof(range_buf), "%d° ~ %d°",
                     static_cast<int>(current_data_.forecast[0].temp_min),
                     static_cast<int>(current_data_.forecast[0].temp_max));
        } else {
            snprintf(range_buf, sizeof(range_buf), "-- ~ --");
        }
        const std::string range = FitTextToWidth(range_buf, font_, 120);
        const int stack_right = icon_x - 12;
        DrawText(fb, width, stack_right - MeasureTextWidth(feels_line.c_str(), font_),
                 InkCenteredTextTopY(font_, feels_line.c_str(), 76, 0),
                 feels_line.c_str(), font_, secondary);
        DrawText(fb, width, stack_right - MeasureTextWidth(range.c_str(), font_),
                 InkCenteredTextTopY(font_, range.c_str(), 104, 0),
                 range.c_str(), font_, secondary);

        // Fact caption: humidity, wind, air quality, centered.
        std::string air_str = current_data_.air_quality.empty() ? "--" : current_data_.air_quality;
        if (current_data_.air_aqi >= 0) air_str = std::to_string(current_data_.air_aqi) + " " + air_str;
        std::string wind_str = current_data_.wind_dir.empty() ? "" : current_data_.wind_dir;
        wind_str += (current_data_.wind_scale.empty() ? "--" : current_data_.wind_scale) + std::string("级");
        const std::string humidity_str = current_data_.humidity.empty() ? "--" : current_data_.humidity;
        const std::string caption = FitTextToWidth(
            "湿度 " + humidity_str + "% ・ " + wind_str + " ・ 空气 " + air_str,
            font_, width - 32);
        DrawCenteredTextInBox(fb, width, {16, 136, width - 32, 18}, caption, font_, secondary);

        // Dressing advice wraps onto up to two lines at the 16px margin; the
        // "穿衣：" prefix only occupies the first line. Rendered in the 12px
        // small font so the full advice fits without truncation.
        const lv_font_t* small_font = &SourceHanSansSC_12;
        if (!current_data_.dress_advice.empty()) {
            const int line1_w = width - 32 - 36;
            std::string first = current_data_.dress_advice;
            std::string second;
            if (MeasureTextWidth(first.c_str(), small_font) > line1_w) {
                first.clear();
                const char* p = current_data_.dress_advice.c_str();
                while (*p) {
                    const char* start = p;
                    utf8_next(&p);
                    std::string next = first;
                    next.append(start, p - start);
                    if (MeasureTextWidth(next.c_str(), small_font) > line1_w) break;
                    first = std::move(next);
                }
                second = current_data_.dress_advice.substr(first.size());
                // Keep closing punctuation at the end of the first line
                // instead of letting a line start with it.
                if (!second.empty()) {
                    static const char* kClosing = "，。、；：？！）】";
                    for (const char* q = kClosing; *q;) {
                        const char* mark = q;
                        utf8_next(&q);
                        const size_t len = static_cast<size_t>(q - mark);
                        if (second.size() >= len && memcmp(second.data(), mark, len) == 0) {
                            first.append(mark, len);
                            second.erase(0, len);
                            break;
                        }
                    }
                }
                if (MeasureTextWidth(second.c_str(), small_font) > width - 32) {
                    second = FitTextToWidth(second, small_font, width - 32);
                }
            }
            const std::string line1 = "穿衣：" + first;
            const int line1_center = second.empty() ? 176 : 169;
            DrawText(fb, width, 16,
                     InkCenteredTextTopY(small_font, line1.c_str(), line1_center, 0),
                     line1.c_str(), small_font, text);
            if (!second.empty()) {
                DrawText(fb, width, 16,
                         InkCenteredTextTopY(small_font, second.c_str(), 183, 0),
                         second.c_str(), small_font, text);
            }
        }

        DrawHLine(fb, width, 200, 16, width - 16, border);

        const std::vector<ForecastRenderItem> forecast_items = BuildForecastItems(current_data_);
        const int forecast_count = static_cast<int>(forecast_items.size());
        if (page_index_ >= forecast_count && forecast_count > 0) {
            page_index_ = forecast_count - 1;
        }

        // Four borderless forecast columns: weekday, icon, high and low temps,
        // separated by dashed vertical hairlines.
        const int col_w = (width - 32) / 4;
        for (int i = 0; i < forecast_count && i < 4; ++i) {
            const auto& item = forecast_items[i];
            const int col_x = 16 + i * col_w;
            const int cx = col_x + col_w / 2;
            if (i > 0) {
                for (int y = 212; y <= 276; y += 4) {
                    set_pixel(fb, width, col_x, y, border);
                }
            }
            DrawCenteredTextInBox(fb, width, {col_x + 2, 206, col_w - 4, 16},
                                  item.label, font_, secondary);
            const char* glyph = IconGlyphForCode(item.icon_code, item.weather_text);
            const int forecast_icon_w = MeasureTextWidth(glyph, &weather_icons_16);
            DrawIcon(fb, width, cx - forecast_icon_w / 2,
                     InkCenteredTextTopY(&weather_icons_16, glyph, 232, 0),
                     glyph, &weather_icons_16, RED);
            char hi_buf[12];
            char lo_buf[12];
            snprintf(hi_buf, sizeof(hi_buf), "%d°", static_cast<int>(item.temp_max));
            snprintf(lo_buf, sizeof(lo_buf), "%d°", static_cast<int>(item.temp_min));
            DrawCenteredTextInBox(fb, width, {col_x + 2, 248, col_w - 4, 16},
                                  hi_buf, font_, text);
            DrawCenteredTextInBox(fb, width, {col_x + 2, 266, col_w - 4, 16},
                                  lo_buf, font_, secondary);
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
