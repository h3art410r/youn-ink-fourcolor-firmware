#include "memo_renderer.h"

#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <string>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;

namespace rawdraw {
namespace {

std::vector<std::string> WrapMemo(const char* text, const lv_font_t* font,
                                  int max_width, size_t max_lines) {
    std::vector<std::string> lines;
    if (!text || !font || max_width <= 0 || max_lines == 0) return lines;
    std::string line;
    const char* p = text;
    while (*p && lines.size() < max_lines) {
        const char* start = p;
        const uint32_t codepoint = utf8_next(&p);
        if (codepoint == '\n') {
            lines.push_back(line);
            line.clear();
            continue;
        }
        std::string next = line;
        next.append(start, p - start);
        if (!line.empty() && MeasureTextWidth(next.c_str(), font) > max_width) {
            lines.push_back(line);
            line.assign(start, p - start);
        } else {
            line = std::move(next);
        }
    }
    if (!line.empty() && lines.size() < max_lines) lines.push_back(line);
    if (*p && !lines.empty()) {
        while (!lines.back().empty() && MeasureTextWidth((lines.back() + "…").c_str(), font) > max_width) {
            const char* begin = lines.back().c_str();
            const char* end = begin;
            while (*end) utf8_next(&end);
            const char* prev = begin;
            while (prev < end) {
                const char* next = prev;
                utf8_next(&next);
                if (next >= end) break;
                prev = next;
            }
            lines.back().resize(static_cast<size_t>(prev - begin));
        }
        lines.back() += "…";
    }
    return lines;
}

}  // namespace

void MemoRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    title_font_ = &SourceHanSansSC_Medium_slim;
    body_font_ = &SourceHanSansSC_Regular_slim;
    small_font_ = &SourceHanSansSC_Regular_slim;
    selected_ = 0;
    Reload();
    needs_full_refresh_ = true;
}

void MemoRenderer::Reload() {
    count_ = 0;
    memo_storage_get_all(items_.data(), items_.size(), &count_);
    if (count_ == 0) selected_ = 0;
    else if (selected_ >= count_) selected_ = 0;
    needs_full_refresh_ = true;
}

bool MemoRenderer::Advance() {
    Reload();
    if (count_ == 0) return false;
    selected_ = (selected_ + 1) % count_;
    needs_full_refresh_ = true;
    return true;
}

bool MemoRenderer::HandleInput(const ButtonEvent&) {
    return false;
}

void MemoRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    const auto& theme = ThemeManager::Get();
    const PaintStyle background = theme.Style(ThemeToken::BackgroundPrimary);
    const PaintStyle panel = theme.Component(ComponentRole::Panel);
    const PaintStyle heading = theme.Style(ThemeToken::TextPrimary);
    const PaintStyle secondary = theme.Style(ThemeToken::TextSecondary);
    const PaintStyle divider = theme.Style(ThemeToken::Border);

    const int top = Style::kStatusBarHeight + 7;
    const int card_x = 14;
    const int card_y = top;
    const int card_w = width - 28;
    const int card_h = height - top - 10;
    DrawStyledRect(fb, width, {0, Style::kStatusBarHeight, width, height - Style::kStatusBarHeight}, background);
    DrawStyledRoundRect(fb, width, height, {card_x, card_y, card_w, card_h},
                        Style::kBorderRadiusMD, panel);

    if (count_ == 0) {
        DrawStyledText(fb, width, card_x + 28, card_y + 50, "暂无备忘", title_font_, heading, height);
        DrawStyledText(fb, width, card_x + 28, card_y + 94,
                       "请打开设备网页添加内容", body_font_, secondary, height);
        DrawStyledText(fb, width, card_x + 28, card_y + 126,
                       "本页用于阅读，内容在后台管理", small_font_, secondary, height);
        return;
    }

    const MemoItem& item = items_[selected_];
    char counter[20];
    snprintf(counter, sizeof(counter), "%u / %u",
             static_cast<unsigned>(selected_ + 1), static_cast<unsigned>(count_));
    DrawStyledText(fb, width, card_x + card_w - 76, card_y + 14, counter, small_font_, secondary, height);
    DrawStyledText(fb, width, card_x + 22, card_y + 16, item.title, title_font_, heading, height);
    DrawStyledRect(fb, width, {card_x + 20, card_y + 48, card_w - 40, 1}, divider);

    const int body_y = card_y + 62;
    const int footer_y = card_y + card_h - 27;
    const int line_height = 22;
    const size_t max_lines = static_cast<size_t>((footer_y - body_y - 8) / line_height);
    const std::string body = item.body[0] ? item.body : "（无正文）";
    const auto lines = WrapMemo(body.c_str(), body_font_, card_w - 48, max_lines);
    for (size_t i = 0; i < lines.size(); ++i) {
        DrawStyledText(fb, width, card_x + 22,
                       body_y + static_cast<int>(i) * line_height,
                       lines[i].c_str(), body_font_, heading, height);
    }
    DrawStyledRect(fb, width, {card_x + 20, footer_y - 7, card_w - 40, 1}, divider);
    DrawStyledText(fb, width, card_x + 22, footer_y, "BOOT 下一条 · 网页可编辑", small_font_, secondary, height);
}

}  // namespace rawdraw
