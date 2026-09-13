#include "lvgl.h"
#include "ui/renderers/rawdraw/weather_renderer.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/theme.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t font_zectrix_16_1;

bool weather_api_fetch_now() { return false; }

namespace {
constexpr int kWidth = 400;
constexpr int kHeight = 300;
constexpr std::array<std::array<uint8_t, 3>, 4> kPalette{{
    {{0, 0, 0}}, {{255, 255, 255}}, {{255, 210, 0}}, {{220, 0, 0}},
}};

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

    // Draw the same two overlays used by the application page compositor.
    const int title_y = rawdraw::InkCenteredTextTopYInBox(
        &SourceHanSansSC_Regular_slim, "天气 1/2", 0, Style::kStatusBarHeight, 0);
    rawdraw::DrawText(fb.data(), kWidth, 155, title_y, "天气 1/2",
                      &SourceHanSansSC_Regular_slim, rawdraw::BLACK, kHeight);
    rawdraw::DrawText(fb.data(), kWidth, 320, 5, "10:08", &font_zectrix_16_1, rawdraw::BLACK, kHeight);

    if (!WritePpm(output, fb)) {
        std::cerr << "cannot write preview: " << output << '\n';
        return 1;
    }
    std::cout << output << '\n';
    return 0;
}
