#!/usr/bin/env python3
"""Render a quick four-color preview of the current 400x300 weather layout.

This is a layout smoke-test, not a pixel-accurate port of WeatherRenderer.
Keep the page regions and sample content aligned with weather_renderer.cc.
"""

from pathlib import Path
import argparse

from PIL import Image, ImageDraw, ImageFont


W, H = 400, 300
BLACK, WHITE, YELLOW, RED = (0, 0, 0), (255, 255, 255), (255, 220, 0), (220, 0, 0)
BG = WHITE
FONT_PATH = Path(r"C:\Windows\Fonts\msyh.ttc")


def font(size, index=0):
    if not FONT_PATH.exists():
        return ImageFont.load_default()
    return ImageFont.truetype(str(FONT_PATH), size, index=index)


def centered(draw, box, text, fnt, fill):
    x, y, w, h = box
    bounds = draw.textbbox((0, 0), text, font=fnt)
    tw, th = bounds[2] - bounds[0], bounds[3] - bounds[1]
    draw.text((x + max(0, (w - tw) // 2), y + (h - th) // 2 - bounds[1]),
              text, font=fnt, fill=fill)


def draw_sun(draw, cx, cy):
    draw.ellipse((cx - 20, cy - 20, cx + 20, cy + 20), fill=RED)
    for dx, dy in ((0, -30), (0, 30), (-30, 0), (30, 0),
                   (-22, -22), (22, -22), (-22, 22), (22, 22)):
        x1, y1 = cx + dx, cy + dy
        if dx == 0:
            y1 += 8 if dy < 0 else -8
            x2, y2 = cx + dx, cy + dy + (8 if dy < 0 else -8)
        elif dy == 0:
            x1 += 8 if dx < 0 else -8
            x2, y2 = cx + dx + (8 if dx < 0 else -8), cy + dy
        else:
            x2, y2 = cx + dx * 1.2, cy + dy * 1.2
        draw.line((x1, y1, x2, y2), fill=RED, width=4)


def render(output):
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    f16, f20, f28, f48 = font(16), font(20), font(28), font(48)

    # Status bar and carousel title, approximating the board's live state.
    d.line((0, 27, W - 1, 27), fill=BLACK, width=1)
    d.text((16, 5), "●  ●", font=f16, fill=BLACK)
    centered(d, (110, 1, 180, 26), "天气 1/2", f16, BLACK)
    d.text((320, 5), "10:08  96%", font=f16, fill=BLACK)

    # Keep these zones in sync with WeatherRenderer::Render.
    centered(d, (16, 30, W - 32, 22), "上海", f20, BLACK)
    d.rounded_rectangle((16, 54, W - 16, 132), radius=12, fill=YELLOW, outline=BLACK, width=1)
    centered(d, (28, 62, 156, 37), "24°", f48, BLACK)
    centered(d, (28, 100, 156, 23), "体感 26°", f16, BLACK)
    draw_sun(d, 270, 84)
    centered(d, (190, 101, 160, 22), "晴", f20, BLACK)

    d.rounded_rectangle((16, 138, W - 16, 190), radius=8, fill=WHITE, outline=BLACK, width=1)
    metrics = (("空气质量", "33 优"), ("湿度", "65%"), ("风力 / 风向", "2级 东风"))
    for i, (label, value) in enumerate(metrics):
        x = 16 + i * 122
        if i:
            d.line((x, 146, x, 182), fill=BLACK, width=1)
        centered(d, (x + 3, 144, 116, 20), label, f16, BLACK)
        centered(d, (x + 3, 168, 116, 20), value, f16, BLACK)

    d.rounded_rectangle((16, 198, W - 16, 260), radius=8, fill=WHITE, outline=BLACK, width=1)
    days = (("今天", "24/18°"), ("周一", "25/19°"), ("周二", "23/17°"), ("周三", "22/16°"))
    card_w = 92
    for i, (day, temps) in enumerate(days):
        x = 17 + i * card_w
        if i:
            for yy in range(207, 252, 4):
                d.point((x, yy), fill=BLACK)
        centered(d, (x + 3, 200, card_w - 6, 17), day, f16, BLACK)
        d.ellipse((x + 38, 222, x + 52, 236), fill=RED)
        centered(d, (x + 3, 240, card_w - 6, 17), temps, f16, BLACK)

    d.line((18, 265, W - 18, 265), fill=BLACK, width=1)
    centered(d, (16, 268, W - 32, 26), "数据来源：和风天气", f16, BLACK)

    # E-paper uses only four inks; quantize accidental anti-alias colors.
    palette = Image.new("P", (1, 1))
    palette.putpalette([*BLACK, *WHITE, *YELLOW, *RED] + [0, 0, 0] * 252)
    im = im.quantize(palette=palette, dither=Image.Dither.NONE).convert("RGB")
    output.parent.mkdir(parents=True, exist_ok=True)
    im.save(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path,
                        default=Path(__file__).parents[1] / "build-s3" / "preview" / "weather.png")
    args = parser.parse_args()
    render(args.output)
    print(args.output.resolve())


if __name__ == "__main__":
    main()
