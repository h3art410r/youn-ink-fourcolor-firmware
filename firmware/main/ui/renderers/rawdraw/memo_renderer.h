#ifndef RAWDRAW_MEMO_RENDERER_H
#define RAWDRAW_MEMO_RENDERER_H

#include "common/memo_storage.h"
#include "page_renderer.h"

#include <array>
#include <stddef.h>

namespace rawdraw {

class MemoRenderer : public PageRenderer {
public:
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;
    bool Advance();
    void Reload();

private:
    std::array<MemoItem, MEMO_MAX_ITEMS> items_{};
    size_t count_ = 0;
    size_t selected_ = 0;
    const lv_font_t* title_font_ = nullptr;
    const lv_font_t* body_font_ = nullptr;
    const lv_font_t* small_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_MEMO_RENDERER_H
