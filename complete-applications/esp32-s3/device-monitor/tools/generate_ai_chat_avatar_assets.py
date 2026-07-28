#!/usr/bin/env python3
"""Generate static AI Chat avatar sprites for LVGL.

The runtime deliberately uses still-frame sprites instead of GIF/video.  That
keeps the AI page cheap while still allowing expression changes driven by the
chat state.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


AVATAR_W = 96
AVATAR_H = 96
PAGE_BG = (232, 243, 250)
TRANSPARENT = (0, 0, 0, 0)
ROLE_COUNT = 2
STATE_NAMES = ("idle", "listening", "thinking", "speaking", "resting", "error")


def sc(value: int) -> int:
    return int(round(value * AVATAR_W / 72.0))


def is_soft_background(pixel: tuple[int, int, int]) -> bool:
    r, g, b = pixel
    spread = max(pixel) - min(pixel)
    if r >= 226 and g >= 232 and b >= 235 and spread <= 46:
        return True
    dr = r - PAGE_BG[0]
    dg = g - PAGE_BG[1]
    db = b - PAGE_BG[2]
    return (dr * dr + dg * dg + db * db) <= 52 * 52 and spread <= 62


def make_background_transparent(image: Image.Image) -> Image.Image:
    rgba = image.convert("RGBA")
    width, height = rgba.size
    pixels = rgba.load()
    seen = [[False] * width for _ in range(height)]
    stack: list[tuple[int, int]] = []

    for x in range(width):
        stack.append((x, 0))
        stack.append((x, height - 1))
    for y in range(height):
        stack.append((0, y))
        stack.append((width - 1, y))

    while stack:
        x, y = stack.pop()
        if x < 0 or y < 0 or x >= width or y >= height or seen[y][x]:
            continue
        seen[y][x] = True
        r, g, b, _a = pixels[x, y]
        if not is_soft_background((r, g, b)):
            continue
        pixels[x, y] = TRANSPARENT
        stack.append((x + 1, y))
        stack.append((x - 1, y))
        stack.append((x, y + 1))
        stack.append((x, y - 1))

    remove_shadow_pixels(rgba)
    return rgba


def is_detached_shadow(pixel: tuple[int, int, int]) -> bool:
    r, g, b = pixel
    spread = max(pixel) - min(pixel)
    if min(pixel) < 72:
        return False
    if r > g + 14 and r > b + 14:
        return False
    if g > r + 28 and g > b + 8:
        return False
    return b >= r + 7 and b >= g - 2 and spread < 72


def is_ground_line(pixel: tuple[int, int, int]) -> bool:
    r, g, b = pixel
    # Generated character sheets sometimes include a light blue floor line under
    # the feet.  It is not part of the role, and looks like a stray underline on
    # the device screen, so strip only this bright-blue lower-region color band.
    return 95 <= r <= 175 and 125 <= g <= 200 and 170 <= b <= 240 and b >= g + 18 and g >= r + 20


def remove_shadow_pixels(image: Image.Image) -> None:
    pixels = image.load()
    width, height = image.size
    start_y = int(height * 0.70)
    for y in range(start_y, height):
        for x in range(width):
            r, g, b, a = pixels[x, y]
            if a != 0 and (is_detached_shadow((r, g, b)) or is_ground_line((r, g, b))):
                pixels[x, y] = TRANSPARENT


def content_bbox(image: Image.Image) -> tuple[int, int, int, int]:
    rgba = image.convert("RGBA")
    pixels = rgba.load()
    width, height = rgba.size
    min_x, min_y = width, height
    max_x, max_y = -1, -1
    for y in range(height):
        for x in range(width):
            _r, _g, _b, a = pixels[x, y]
            if a != 0:
                min_x = min(min_x, x)
                min_y = min(min_y, y)
                max_x = max(max_x, x)
                max_y = max(max_y, y)
    if max_x < 0:
        return (0, 0, width - 1, height - 1)
    return (min_x, min_y, max_x, max_y)


def fit_avatar(crop: Image.Image, margin: int = 2, y_bias: int = 0) -> Image.Image:
    normalized = make_background_transparent(crop)
    bbox = content_bbox(normalized)
    subject = normalized.crop((bbox[0], bbox[1], bbox[2] + 1, bbox[3] + 1))
    max_w = AVATAR_W - margin * 2
    max_h = AVATAR_H - margin * 2
    scale = min(max_w / subject.width, max_h / subject.height)
    out_w = max(1, int(round(subject.width * scale)))
    out_h = max(1, int(round(subject.height * scale)))
    resized = subject.resize((out_w, out_h), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (AVATAR_W, AVATAR_H), TRANSPARENT)
    x = (AVATAR_W - out_w) // 2
    y = (AVATAR_H - out_h) // 2 + y_bias
    y = max(0, min(AVATAR_H - out_h, y))
    canvas.paste(resized, (x, y), resized)
    return canvas


def add_buddy_speaking_marks(image: Image.Image) -> None:
    draw = ImageDraw.Draw(image)
    green = (35, 193, 125, 255)
    draw.line((sc(56), sc(30), sc(64), sc(26)), fill=green, width=sc(2))
    draw.line((sc(58), sc(38), sc(67), sc(38)), fill=green, width=sc(2))
    draw.line((sc(56), sc(46), sc(64), sc(51)), fill=green, width=sc(2))


def crop_buddy_states(source: Image.Image) -> list[Image.Image]:
    # The generated design has six small expressions inside the dashed carousel.
    # Crop the actual expression frames, not the large preview image.
    width, height = source.size
    sx = width / 1536.0
    sy = height / 1152.0
    centers = [
        (664, 548),   # listening / attentive
        (804, 548),   # thinking / eyes relaxed
        (950, 548),   # idle
        (1098, 548),  # speaking / signal marks
        (1240, 548),  # alert
        (1376, 548),  # resting
    ]
    crop_w = int(round(124 * sx))
    crop_h = int(round(152 * sy))

    frames = []
    for cx, cy in centers:
        cx = int(round(cx * sx))
        cy = int(round(cy * sy))
        left = max(0, cx - crop_w // 2)
        top = max(0, cy - crop_h // 2)
        frames.append(source.crop((left, top, left + crop_w, top + crop_h)))

    listening = fit_avatar(frames[0], margin=5, y_bias=3)
    thinking = fit_avatar(frames[1], margin=5, y_bias=3)
    idle = fit_avatar(frames[2], margin=5, y_bias=3)
    speaking = idle.copy()
    add_buddy_speaking_marks(speaking)
    resting = fit_avatar(frames[5], margin=5, y_bias=3)
    error = fit_avatar(frames[4], margin=5, y_bias=3)
    add_error_badge(error)
    return [idle, listening, thinking, speaking, resting, error]


def crop_sprout_base(source: Image.Image) -> Image.Image:
    width, height = source.size
    sx = width / 1536.0
    sy = height / 1097.0
    box = (
        int(round(515 * sx)),
        int(round(260 * sy)),
        int(round(1018 * sx)),
        int(round(646 * sy)),
    )
    return fit_avatar(source.crop(box), margin=3, y_bias=3)


def repaint_cat_eyes(draw: ImageDraw.ImageDraw, closed: bool) -> None:
    skin = (252, 254, 255, 255)
    if closed:
        draw.rectangle((sc(20), sc(28), sc(29), sc(36)), fill=skin)
        draw.rectangle((sc(42), sc(28), sc(51), sc(36)), fill=skin)
        draw.line((sc(21), sc(32), sc(29), sc(35)), fill=(14, 42, 77, 255), width=sc(2))
        draw.line((sc(42), sc(35), sc(50), sc(32)), fill=(14, 42, 77, 255), width=sc(2))


def add_listening_marks(image: Image.Image) -> None:
    draw = ImageDraw.Draw(image)
    green = (35, 193, 125, 255)
    draw.arc((sc(12), sc(12), sc(27), sc(28)), 200, 290, fill=green, width=sc(2))
    draw.arc((sc(8), sc(9), sc(30), sc(31)), 198, 292, fill=green, width=sc(2))
    draw.arc((sc(45), sc(12), sc(60), sc(28)), 250, 340, fill=green, width=sc(2))
    draw.arc((sc(42), sc(9), sc(64), sc(31)), 248, 342, fill=green, width=sc(2))


def clear_cat_source_marks(image: Image.Image) -> None:
    pixels = image.load()
    for y in range(0, sc(28)):
        for x in range(0, AVATAR_W):
            r, g, b, a = pixels[x, y]
            if a != 0 and g > 145 and r < 95 and b < 150:
                pixels[x, y] = TRANSPARENT


def add_thinking_marks(image: Image.Image) -> None:
    draw = ImageDraw.Draw(image)
    blue = (100, 117, 138, 255)
    for x, y, r in ((28, 9, 2), (36, 7, 3), (46, 10, 2)):
        draw.ellipse((sc(x - r), sc(y - r), sc(x + r), sc(y + r)), fill=blue)


def add_speaking_mouth(image: Image.Image) -> None:
    draw = ImageDraw.Draw(image)
    navy = (16, 41, 74, 255)
    mouth_dx = -3
    mouth_dy = 7
    draw.ellipse((sc(32) + mouth_dx,
                  sc(37) + mouth_dy,
                  sc(40) + mouth_dx,
                  sc(43) + mouth_dy), fill=navy)
    draw.rectangle((sc(34) + mouth_dx,
                    sc(38) + mouth_dy,
                    sc(38) + mouth_dx,
                    sc(40) + mouth_dy), fill=(255, 192, 203, 255))
    green = (35, 193, 125, 255)
    draw.line((sc(57), sc(32), sc(63), sc(29)), fill=green, width=sc(2))
    draw.line((sc(58), sc(38), sc(65), sc(38)), fill=green, width=sc(2))
    draw.line((sc(57), sc(44), sc(63), sc(48)), fill=green, width=sc(2))


def add_rest_marks(image: Image.Image) -> None:
    draw = ImageDraw.Draw(image)
    repaint_cat_eyes(draw, True)
    draw.rectangle((sc(6), sc(5), sc(28), sc(25)), fill=TRANSPARENT)
    draw.rectangle((sc(44), sc(5), sc(68), sc(25)), fill=TRANSPARENT)
    draw.text((sc(52), sc(8)), "Z", fill=(100, 117, 138, 255))
    draw.text((sc(60), sc(2)), "z", fill=(100, 117, 138, 255))


def add_error_badge(image: Image.Image) -> None:
    draw = ImageDraw.Draw(image)
    red = (228, 87, 87, 255)
    draw.ellipse((sc(54), sc(7), sc(66), sc(19)), fill=red)
    draw.rectangle((sc(59), sc(10), sc(61), sc(15)), fill=(255, 255, 255, 255))
    draw.point((sc(60), sc(17)), fill=(255, 255, 255, 255))


def crop_sprout_states(source: Image.Image) -> list[Image.Image]:
    base = crop_sprout_base(source)
    idle = base.copy()
    clear_cat_source_marks(idle)
    listening = base.copy()
    add_listening_marks(listening)
    thinking = base.copy()
    clear_cat_source_marks(thinking)
    add_thinking_marks(thinking)
    speaking = base.copy()
    clear_cat_source_marks(speaking)
    add_speaking_mouth(speaking)
    resting = base.copy()
    add_rest_marks(resting)
    error = base.copy()
    clear_cat_source_marks(error)
    add_error_badge(error)
    return [idle, listening, thinking, speaking, resting, error]


def rgb565_alpha_bytes(image: Image.Image) -> list[int]:
    data: list[int] = []
    rgba = image.convert("RGBA").tobytes()
    for pos in range(0, len(rgba), 4):
        r, g, b, a = rgba[pos], rgba[pos + 1], rgba[pos + 2], rgba[pos + 3]
        value = 0 if a == 0 else ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        # LVGL stores LV_IMG_CF_TRUE_COLOR_ALPHA bytes in the in-memory
        # lv_color_t layout. This project enables LV_COLOR_16_SWAP, so the
        # RGB565 color bytes must be written high-byte first, then low-byte.
        data.append((value >> 8) & 0xFF)
        data.append(value & 0xFF)
        data.append(a)
    return data


def c_array(name: str, data: list[int]) -> list[str]:
    lines = [f"static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {name}[] = {{"]
    for pos in range(0, len(data), 16):
        chunk = ", ".join(f"0x{byte:02X}" for byte in data[pos:pos + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    return lines


def write_assets_c(path: Path, roles: dict[str, list[Image.Image]]) -> None:
    lines: list[str] = [
        '#include "ai_chat_avatar_assets.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
        "#ifndef LV_ATTRIBUTE_LARGE_CONST",
        "#define LV_ATTRIBUTE_LARGE_CONST",
        "#endif",
        "",
        "#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP == 0",
        '#error "ai_chat_avatar_assets.c expects LV_COLOR_DEPTH=16 and LV_COLOR_16_SWAP=1"',
        "#endif",
        "",
        "/* RGB565 pixels are encoded in LVGL's swapped 16-bit color layout: high byte, low byte, alpha. */",
        "",
    ]
    descriptors: list[str] = []
    role_keys = ("buddy", "sprout")
    for role in role_keys:
        for state, image in zip(STATE_NAMES, roles[role]):
            map_name = f"ai_chat_avatar_{role}_{state}_map"
            img_name = f"ai_chat_avatar_{role}_{state}_img"
            lines.extend(c_array(map_name, rgb565_alpha_bytes(image)))
            lines.extend([
                f"const lv_img_dsc_t {img_name} = {{",
                "    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,",
                "    .header.always_zero = 0,",
                "    .header.reserved = 0,",
                f"    .header.w = {AVATAR_W},",
                f"    .header.h = {AVATAR_H},",
                f"    .data_size = sizeof({map_name}),",
                f"    .data = {map_name},",
                "};",
                "",
            ])
            descriptors.append(f"&{img_name}")

    lines.append("static const lv_img_dsc_t *const s_avatar_images[AI_CHAT_AVATAR_ROLE_COUNT][AI_CHAT_AVATAR_STATE_COUNT] = {")
    index = 0
    for role in role_keys:
        lines.append(f"    /* {role} */")
        lines.append("    {")
        for _state in STATE_NAMES:
            lines.append(f"        {descriptors[index]},")
            index += 1
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("const lv_img_dsc_t *ai_chat_avatar_asset_get(uint8_t role, ai_chat_avatar_state_t state)")
    lines.append("{")
    lines.append("    if (role >= AI_CHAT_AVATAR_ROLE_COUNT) {")
    lines.append("        role = AI_CHAT_AVATAR_ROLE_BUDDY;")
    lines.append("    }")
    lines.append("    if ((uint8_t)state >= AI_CHAT_AVATAR_STATE_COUNT) {")
    lines.append("        state = AI_CHAT_AVATAR_STATE_IDLE;")
    lines.append("    }")
    lines.append("    return s_avatar_images[role][state];")
    lines.append("}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def write_assets_h(path: Path) -> None:
    path.write_text(
        f"""#pragma once

#include <stdint.h>

#include "lvgl.h"

#define AI_CHAT_AVATAR_ASSET_WIDTH  {AVATAR_W}
#define AI_CHAT_AVATAR_ASSET_HEIGHT {AVATAR_H}
#define AI_CHAT_AVATAR_ROLE_COUNT   2U

typedef enum {{
    AI_CHAT_AVATAR_ROLE_BUDDY = 0,
    AI_CHAT_AVATAR_ROLE_SPROUT = 1,
}} ai_chat_avatar_role_t;

typedef enum {{
    AI_CHAT_AVATAR_STATE_IDLE = 0,
    AI_CHAT_AVATAR_STATE_LISTENING,
    AI_CHAT_AVATAR_STATE_THINKING,
    AI_CHAT_AVATAR_STATE_SPEAKING,
    AI_CHAT_AVATAR_STATE_RESTING,
    AI_CHAT_AVATAR_STATE_ERROR,
    AI_CHAT_AVATAR_STATE_COUNT,
}} ai_chat_avatar_state_t;

const lv_img_dsc_t *ai_chat_avatar_asset_get(uint8_t role, ai_chat_avatar_state_t state);
""",
        encoding="utf-8",
    )


def write_preview(preview_dir: Path, roles: dict[str, list[Image.Image]]) -> None:
    preview_dir.mkdir(parents=True, exist_ok=True)
    for role, frames in roles.items():
        for state, image in zip(STATE_NAMES, frames):
            image.save(preview_dir / f"ai_chat_avatar_{role}_{state}.png")

    cell_w = AVATAR_W + 18
    cell_h = AVATAR_H + 26
    sheet = Image.new("RGB", (cell_w * len(STATE_NAMES), cell_h * ROLE_COUNT), PAGE_BG)
    draw = ImageDraw.Draw(sheet)
    for row, role in enumerate(("buddy", "sprout")):
        for col, state in enumerate(STATE_NAMES):
            x = col * cell_w + 9
            y = row * cell_h + 4
            sheet.paste(roles[role][col], (x, y), roles[role][col])
            draw.text((col * cell_w + 4, y + AVATAR_H + 2), state, fill=(17, 35, 60))
    sheet.save(preview_dir / "ai_chat_avatar_contact_sheet.png")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--buddy-source", required=True, type=Path)
    parser.add_argument("--sprout-source", required=True, type=Path)
    parser.add_argument("--out-c", default=Path("main/ui/ai_chat_avatar_assets.c"), type=Path)
    parser.add_argument("--out-h", default=Path("main/ui/ai_chat_avatar_assets.h"), type=Path)
    parser.add_argument("--preview-dir", default=Path("docs/debug/ai_chat_avatar"), type=Path)
    args = parser.parse_args()

    buddy_source = Image.open(args.buddy_source).convert("RGB")
    sprout_source = Image.open(args.sprout_source).convert("RGB")
    roles = {
        "buddy": crop_buddy_states(buddy_source),
        "sprout": crop_sprout_states(sprout_source),
    }
    write_assets_h(args.out_h)
    write_assets_c(args.out_c, roles)
    write_preview(args.preview_dir, roles)
    print(f"generated {ROLE_COUNT * len(STATE_NAMES)} avatar frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
