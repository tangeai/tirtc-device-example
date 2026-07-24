#!/usr/bin/env python3
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
ASSETS = (
    (
        "call_contacts_refresh_default_img",
        ROOT / "main/ui/image/call_contacts_refresh_default.png",
        "Figma node 143:58",
    ),
    (
        "call_contacts_refresh_pressed_img",
        ROOT / "main/ui/image/call_contacts_refresh_pressed.png",
        "Figma node 143:60",
    ),
)
OUTPUT_C = ROOT / "main/ui/call_assets.c"
OUTPUT_H = ROOT / "main/ui/call_assets.h"
EXPECTED_SIZE = (50, 22)


def rgb565_alpha_bytes(image: Image.Image) -> list[int]:
    data: list[int] = []
    rgba = image.convert("RGBA").tobytes()
    for pos in range(0, len(rgba), 4):
        r, g, b, a = rgba[pos], rgba[pos + 1], rgba[pos + 2], rgba[pos + 3]
        value = 0 if a == 0 else ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data.extend(((value >> 8) & 0xFF, value & 0xFF, a))
    return data


def append_c_array(lines: list[str], name: str, data: list[int]) -> None:
    lines.append(f"static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {name}[] = {{")
    for pos in range(0, len(data), 16):
        chunk = ", ".join(f"0x{byte:02X}" for byte in data[pos : pos + 16])
        lines.append(f"    {chunk},")
    lines.extend(("};", ""))


def main() -> int:
    c_lines = [
        '#include "call_assets.h"',
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
        '#error "call_assets.c expects LV_COLOR_DEPTH=16 and LV_COLOR_16_SWAP=1"',
        "#endif",
        "",
        "/* Generated from exact Figma PNG exports. Do not redraw these labels in LVGL. */",
        "",
    ]

    symbols: list[str] = []
    for symbol, source, provenance in ASSETS:
        image = Image.open(source).convert("RGBA")
        if image.size != EXPECTED_SIZE:
            raise ValueError(f"{source} has size {image.size}, expected {EXPECTED_SIZE}")
        map_name = f"{symbol}_map"
        c_lines.append(f"/* {provenance}: {source.relative_to(ROOT).as_posix()} */")
        append_c_array(c_lines, map_name, rgb565_alpha_bytes(image))
        c_lines.extend(
            (
                f"const lv_img_dsc_t {symbol} = {{",
                "    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,",
                "    .header.always_zero = 0,",
                "    .header.reserved = 0,",
                f"    .header.w = {image.width},",
                f"    .header.h = {image.height},",
                f"    .data_size = sizeof({map_name}),",
                f"    .data = {map_name},",
                "};",
                "",
            )
        )
        symbols.append(symbol)

    OUTPUT_C.write_text("\n".join(c_lines), encoding="utf-8", newline="\n")
    OUTPUT_H.write_text(
        "#pragma once\n\n"
        '#include "lvgl.h"\n\n'
        + "\n".join(f"LV_IMG_DECLARE({symbol});" for symbol in symbols)
        + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
