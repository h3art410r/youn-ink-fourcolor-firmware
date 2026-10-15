#include "lvgl.h"
#include "ui/renderers/rawdraw/weather_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;

bool weather_api_fetch_now() { return false; }

namespace {
constexpr int kWidth = 400;
constexpr int kHeight = 300;
constexpr std::array<std::array<uint8_t, 3>, 4> kPalette{{
    {{0, 0, 0}}, {{255, 255, 255}}, {{255, 210, 0}}, {{220, 0, 0}},
}};

void DrawMiniTimeDigit(uint8_t* fb, int x, int y, char digit, rawdraw::Color color) {
    static constexpr uint8_t kSegments[10] = {
        0b1111110, 0b0110000, 0b1101101, 0b1111001, 0b0110011,
        0b1011011, 0b1011111, 0b1110000, 0b1111111, 0b1111011,
    };
    if (digit < '0' || digit > '9') return;
    const uint8_t seg = kSegments[digit - '0'];
    if (seg & 0b1000000) rawdraw::DrawHLine(fb, kWidth, y, x + 1, x + 4, color);
    if (seg & 0b0100000) rawdraw::DrawVLine(fb, kWidth, x + 5, y + 1, y + 4, color);
    if (seg & 0b0010000) rawdraw::DrawVLine(fb, kWidth, x + 5, y + 6, y + 9, color);
    if (seg & 0b0001000) rawdraw::DrawHLine(fb, kWidth, y + 10, x + 1, x + 4, color);
    if (seg & 0b0000100) rawdraw::DrawVLine(fb, kWidth, x, y + 6, y + 9, color);
    if (seg & 0b0000010) rawdraw::DrawVLine(fb, kWidth, x, y + 1, y + 4, color);
    if (seg & 0b0000001) rawdraw::DrawHLine(fb, kWidth, y + 5, x + 1, x + 4, color);
}

void DrawPreviewStatusBar(uint8_t* fb) {
    using namespace rawdraw;
    constexpr int kBarHeight = Style::kStatusBarHeight;
    const auto& theme = ThemeManager::Get();
    const auto background = theme.Style(ThemeToken::BackgroundPrimary);
    const auto border = theme.Style(ThemeToken::Border);
    const auto text = theme.Style(ThemeToken::TextPrimary);
    const auto success = theme.Style(ThemeToken::SuccessLike);
    const auto accent = theme.Style(ThemeToken::Accent);

    DrawStyledRect(fb, kWidth, {0, 0, kWidth, kBarHeight}, background);
    DrawRect(fb, kWidth, {1, kBarHeight - Style::kShellDividerThickness,
                          kWidth - 2, Style::kShellDividerThickness}, border.border);

    const int center_y = kBarHeight / 2;
    const int signal_x = Style::kStatusBarPadding + 3;
    constexpr int kSignalHeights[] = {6, 9, 12, 15};
    for (int i = 0; i < 4; ++i) {
        DrawRect(fb, kWidth, {signal_x + i * 5, center_y + 7 - kSignalHeights[i], 3,
                              kSignalHeights[i]}, success.fg);
    }
    // The small outlined marker is the status bar's Wi-Fi-connected/service
    // marker, separate from the four signal bars.
    DrawRectBorder(fb, kWidth, {32, center_y - 3, 7, 7}, 1, text.fg);
    const char* date = "10月15日";
    DrawText(fb, kWidth, 49,
             InkCenteredTextTopY(&SourceHanSansSC_Regular_slim, date, center_y, 0),
             date, &SourceHanSansSC_Regular_slim, text.fg);

    // Fixed preview values make the right-hand clock and battery visible even
    // when the host simulator is offline.
    constexpr int battery_x = kWidth - Style::kStatusBarPadding - 30 + 4;
    constexpr int battery_y = center_y - 6;
    DrawRectBorder(fb, kWidth, {battery_x, battery_y, 24, 12}, 1, text.fg);
    DrawRect(fb, kWidth, {battery_x + 24, battery_y + 3, 2, 6}, text.fg);
    DrawRect(fb, kWidth, {battery_x + 2, battery_y + 2, 6, 8}, text.fg);
    DrawRect(fb, kWidth, {battery_x + 9, battery_y + 2, 6, 8}, text.fg);

    const char* time = "10:08";
    int cursor_x = battery_x - 8 - 32;
    for (int i = 0; i < 5; ++i) {
        if (time[i] == ':') {
            set_pixel(fb, kWidth, cursor_x, center_y - 2, accent.fg);
            set_pixel(fb, kWidth, cursor_x, center_y + 2, accent.fg);
            cursor_x += 4;
        } else {
            DrawMiniTimeDigit(fb, cursor_x, center_y - 5, time[i], accent.fg);
            cursor_x += 7;
        }
    }

    const char* title = "天气 1/2";
    const int title_width = MeasureTextWidth(title, &SourceHanSansSC_Regular_slim);
    constexpr int kRightSafe = 326;
    DrawText(fb, kWidth, kRightSafe - title_width,
             InkCenteredTextTopYInBox(&SourceHanSansSC_Regular_slim, title, 0, kBarHeight, 0),
             title, &SourceHanSansSC_Regular_slim, text.fg);
}

bool WritePpm(const char* filename, const std::vector<uint8_t>& fb) {
    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const auto color = rawdraw::get_pixel(fb.data(), kWidth, x, y);
            out.write(reinterpret_cast<const char*>(kPalette[static_cast<int>(color)].data()), 3);
        }
    }
    return static_cast<bool>(out);
}
}

int main(int argc, char** argv) {
    const char* output = argc > 1 ? argv[1] : "weather-preview.ppm";
    constexpr int kStride = (kWidth * 2 + 7) / 8;
    std::vector<uint8_t> fb(kStride * kHeight);
    rawdraw::SetFramebufferHeightHint(kHeight);
    rawdraw::Clear(fb.data(), kWidth, kHeight, rawdraw::WHITE);

    WeatherData sample{};
    sample.city = "上海";
    sample.source = "QWeather";
    sample.temp = "24";
    sample.feels_like = "26";
    sample.weather_icon = "100";
    sample.weather_text = "晴";
    sample.wind_dir = "东风";
    sample.wind_scale = "2";
    sample.humidity = "65";
    sample.air_quality = "优";
    sample.air_aqi = 33;
    sample.dress_advice = "天气较冷，建议着厚外套、厚牛仔服、厚毛衣、皮衣等保暖服装。年老体弱者宜着大衣呢。";
    sample.forecast = {
        {"今天", "晴", "100", 18, 24},
        {"周一", "多云", "101", 19, 25},
        {"周二", "小雨", "305", 17, 23},
        {"周三", "阴", "104", 16, 22},
    };

    rawdraw::WeatherRenderer renderer;
    renderer.Init(kWidth, kHeight);
    renderer.SetCityName("上海");
    renderer.Update(sample);
    renderer.Render(fb.data(), kWidth, kHeight);

    DrawPreviewStatusBar(fb.data());

    if (!WritePpm(output, fb)) {
        std::cerr << "cannot write preview: " << output << '\n';
        return 1;
    }
    std::cout << output << '\n';
    return 0;
}
