#!/usr/bin/env python3
"""Convert the exact S3 AI avatar PNG frames to G32 LVGL 32-bit assets."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


WIDTH = 96
HEIGHT = 96
ROLES = ("buddy", "sprout")
STATES = ("idle", "listening", "thinking", "speaking", "resting", "error")


def write_text_lf(path: Path, content: str) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write(content)


def bgra_bytes(path: Path) -> list[int]:
    image = Image.open(path).convert("RGBA")
    if image.size != (WIDTH, HEIGHT):
        raise ValueError(f"{path} is {image.size}, expected {(WIDTH, HEIGHT)}")

    output: list[int] = []
    rgba = image.tobytes()
    for offset in range(0, len(rgba), 4):
        red, green, blue, alpha = rgba[offset : offset + 4]
        if alpha == 0:
            red = green = blue = 0
        # LVGL 8 stores lv_color32_t as blue, green, red, alpha in memory.
        output.extend((blue, green, red, alpha))
    return output


def emit_array(name: str, data: list[int]) -> list[str]:
    lines = [
        f"static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {name}[] = {{"
    ]
    for offset in range(0, len(data), 16):
        values = ", ".join(f"0x{value:02X}" for value in data[offset : offset + 16])
        lines.append(f"    {values},")
    lines.extend(("};", ""))
    return lines


def write_header(path: Path) -> None:
    write_text_lf(
        path,
        f"""#pragma once

#include <stdint.h>

#include "third_party/lvgl/lvgl/lvgl.h"

#define AI_CHAT_AVATAR_ASSET_WIDTH  {WIDTH}
#define AI_CHAT_AVATAR_ASSET_HEIGHT {HEIGHT}
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
    )


def write_source(path: Path, source_dir: Path) -> None:
    lines = [
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
        "#if LV_COLOR_DEPTH != 32",
        '#error "ai_chat_avatar_assets.c expects LV_COLOR_DEPTH=32"',
        "#endif",
        "",
        "/* Exact S3 RGBA frames encoded as G32 LVGL blue, green, red, alpha bytes. */",
        "",
    ]
    descriptors: list[str] = []
    for role in ROLES:
        for state in STATES:
            stem = f"ai_chat_avatar_{role}_{state}"
            map_name = f"{stem}_map"
            image_path = source_dir / f"{stem}.png"
            if not image_path.is_file():
                raise FileNotFoundError(image_path)
            lines.extend(emit_array(map_name, bgra_bytes(image_path)))
            lines.extend(
                (
                    f"const lv_img_dsc_t {stem}_img = {{",
                    "    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,",
                    "    .header.always_zero = 0,",
                    "    .header.reserved = 0,",
                    f"    .header.w = {WIDTH},",
                    f"    .header.h = {HEIGHT},",
                    f"    .data_size = sizeof({map_name}),",
                    f"    .data = {map_name},",
                    "};",
                    "",
                )
            )
            descriptors.append(f"&{stem}_img")

    lines.append(
        "static const lv_img_dsc_t *const "
        "s_avatar_images[AI_CHAT_AVATAR_ROLE_COUNT][AI_CHAT_AVATAR_STATE_COUNT] = {"
    )
    descriptor_index = 0
    for role in ROLES:
        lines.extend((f"    /* {role} */", "    {"))
        for _state in STATES:
            lines.append(f"        {descriptors[descriptor_index]},")
            descriptor_index += 1
        lines.append("    },")
    lines.extend(
        (
            "};",
            "",
            "const lv_img_dsc_t *ai_chat_avatar_asset_get(uint8_t role, ai_chat_avatar_state_t state)",
            "{",
            "    if (role >= AI_CHAT_AVATAR_ROLE_COUNT) {",
            "        role = AI_CHAT_AVATAR_ROLE_BUDDY;",
            "    }",
            "    if ((uint8_t)state >= AI_CHAT_AVATAR_STATE_COUNT) {",
            "        state = AI_CHAT_AVATAR_STATE_IDLE;",
            "    }",
            "    return s_avatar_images[role][state];",
            "}",
            "",
        )
    )
    write_text_lf(path, "\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--out-c", required=True, type=Path)
    parser.add_argument("--out-h", required=True, type=Path)
    args = parser.parse_args()

    args.out_c.parent.mkdir(parents=True, exist_ok=True)
    args.out_h.parent.mkdir(parents=True, exist_ok=True)
    write_header(args.out_h)
    write_source(args.out_c, args.source_dir)
    print(f"generated {len(ROLES) * len(STATES)} G32 LVGL avatar frames")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
