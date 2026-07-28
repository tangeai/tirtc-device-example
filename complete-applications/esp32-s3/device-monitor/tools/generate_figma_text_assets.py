#!/usr/bin/env python3
"""Generate LVGL alpha-mask text assets from a Figma PNG atlas.

The atlas is built in Figma with one white text node per row on a transparent
background. The generated image descriptors use LV_IMG_CF_ALPHA_8BIT so the UI
can recolor text at runtime without storing duplicate color variants.
"""

from __future__ import annotations

import argparse
import ast
import json
import re
from pathlib import Path

from PIL import Image


SIZES = (10, 12, 16)


def line_height_for(size: int) -> int:
    if size <= 10:
        return 14
    if size <= 12:
        return 16
    return 20


def c_escape(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
    )


def array_name(index: int, size: int) -> str:
    return f"ui_text_{index:03d}_{size}_img"


def map_name(index: int, size: int) -> str:
    return f"ui_text_{index:03d}_{size}_map"


def extract_cjk_strings(source: Path) -> list[str]:
    data = source.read_text(encoding="utf-8")
    results: list[str] = []
    for match in re.finditer(r'"(?:\\.|[^"\\])*"', data):
        literal = match.group(0)
        try:
            value = ast.literal_eval(literal)
        except Exception:
            value = literal[1:-1]
        if re.search(r"[\u4e00-\u9fff]", value) and value not in results:
            results.append(value)
    return results


def alpha_bbox(alpha, row_y: int, row_h: int, atlas_w: int):
    min_x = atlas_w
    min_y = row_h
    max_x = -1
    max_y = -1
    for y in range(row_h):
        py = row_y + y
        for x in range(atlas_w):
            if alpha.getpixel((x, py)) != 0:
                if x < min_x:
                    min_x = x
                if y < min_y:
                    min_y = y
                if x > max_x:
                    max_x = x
                if y > max_y:
                    max_y = y
    if max_x < 0:
        return None
    return min_x, min_y, max_x, max_y


def build_assets(strings: list[str], atlas: Image.Image):
    alpha = atlas.getchannel("A")
    atlas_w, atlas_h = atlas.size
    assets = []
    y = 0

    for index, text in enumerate(strings):
        for size in SIZES:
            row_h = text.count("\n") + 1
            row_h = row_h * line_height_for(size) + 8
            if y + row_h > atlas_h:
                raise RuntimeError(
                    f"atlas is too short at string #{index} size {size}: "
                    f"need y={y + row_h}, atlas_h={atlas_h}"
                )

            bbox = alpha_bbox(alpha, y, row_h, atlas_w)
            if bbox is None:
                width = 1
                height = 1
                x_offset = 0
                y_offset = 0
                pixels = [0]
            else:
                min_x, min_y, max_x, max_y = bbox
                width = max_x - min_x + 1
                height = max_y - min_y + 1
                x_offset = min_x - 4
                y_offset = min_y - 4
                pixels = [
                    alpha.getpixel((x, y + yy))
                    for yy in range(min_y, max_y + 1)
                    for x in range(min_x, max_x + 1)
                ]

            assets.append(
                {
                    "index": index,
                    "text": text,
                    "size": size,
                    "width": width,
                    "height": height,
                    "x_offset": x_offset,
                    "y_offset": y_offset,
                    "pixels": pixels,
                }
            )
            y += row_h + 2

    if y - 2 != atlas_h:
        raise RuntimeError(f"atlas height mismatch: expected {y - 2}, got {atlas_h}")

    return assets


def load_extra_asset_specs(extra_dir: Path) -> list[dict]:
    manifest = extra_dir / "extra_text_assets.json"
    if not manifest.exists():
        return []
    data = json.loads(manifest.read_text(encoding="utf-8"))
    specs = data.get("assets", [])
    for spec in specs:
        if "text" not in spec or "sizes" not in spec:
            raise RuntimeError(f"invalid extra text asset spec: {spec}")
    return specs


def build_extra_assets(specs: list[dict], start_index: int, extra_dir: Path) -> list[dict]:
    assets: list[dict] = []
    index = start_index
    for spec in specs:
        text = spec["text"]
        by_size = {int(item["size"]): item for item in spec["sizes"]}
        for size in SIZES:
            item = by_size.get(size)
            if item is None:
                raise RuntimeError(f"extra text asset missing size {size}: {text}")
            image_path = extra_dir / item["file"]
            image = Image.open(image_path).convert("RGBA")
            width, height = image.size
            alpha = image.getchannel("A")
            if hasattr(alpha, "get_flattened_data"):
                pixels = list(alpha.get_flattened_data())
            else:
                pixels = list(alpha.getdata())
            assets.append(
                {
                    "index": index,
                    "text": text,
                    "size": size,
                    "width": width,
                    "height": height,
                    "x_offset": int(item.get("x_offset", 0)),
                    "y_offset": int(item.get("y_offset", 0)),
                    "pixels": pixels,
                }
            )
        index += 1
    return assets


def write_c_file(path: Path, assets: list[dict]) -> None:
    lines: list[str] = [
        '#include "text_assets.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
    ]

    for asset in assets:
        symbol = array_name(asset["index"], asset["size"])
        data = asset["pixels"]
        lines.append(
            f"static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t "
            f"{map_name(asset['index'], asset['size'])}[] = {{"
        )
        for pos in range(0, len(data), 16):
            chunk = ", ".join(f"0x{byte:02x}" for byte in data[pos : pos + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
        lines.append(f"static const lv_img_dsc_t {symbol} = {{")
        lines.append("    .header.cf = LV_IMG_CF_ALPHA_8BIT,")
        lines.append("    .header.always_zero = 0,")
        lines.append("    .header.reserved = 0,")
        lines.append(f"    .header.w = {asset['width']},")
        lines.append(f"    .header.h = {asset['height']},")
        lines.append(f"    .data_size = {asset['width']} * {asset['height']},")
        lines.append(f"    .data = {map_name(asset['index'], asset['size'])},")
        lines.append("};")
        lines.append("")

    lines.append("static const ui_text_asset_t s_text_assets[] = {")
    for asset in assets:
        lines.append(
            "    {"
            f"\"{c_escape(asset['text'])}\", "
            f"{asset['size']}, "
            f"{asset['x_offset']}, "
            f"{asset['y_offset']}, "
            f"&{array_name(asset['index'], asset['size'])}"
            "},"
        )
    lines.append("};")
    lines.append("")
    lines.append("uint8_t ui_text_asset_normalize_size(uint8_t size)")
    lines.append("{")
    lines.append("    if (size <= 10U) {")
    lines.append("        return 10U;")
    lines.append("    }")
    lines.append("    if (size <= 13U) {")
    lines.append("        return 12U;")
    lines.append("    }")
    lines.append("    return 16U;")
    lines.append("}")
    lines.append("")
    lines.append("const ui_text_asset_t *ui_text_asset_find(const char *text, uint8_t size)")
    lines.append("{")
    lines.append("    if (text == NULL || text[0] == '\\0') {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    uint8_t normalized = ui_text_asset_normalize_size(size);")
    lines.append("    for (size_t i = sizeof(s_text_assets) / sizeof(s_text_assets[0]); i > 0; --i) {")
    lines.append("        const ui_text_asset_t *asset = &s_text_assets[i - 1];")
    lines.append("        if (asset->size == normalized && strcmp(asset->text, text) == 0) {")
    lines.append("            return asset;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return NULL;")
    lines.append("}")
    lines.append("")
    lines.append("bool ui_text_asset_has_cjk(const char *text)")
    lines.append("{")
    lines.append("    if (text == NULL) {")
    lines.append("        return false;")
    lines.append("    }")
    lines.append("    const unsigned char *p = (const unsigned char *)text;")
    lines.append("    while (*p != '\\0') {")
    lines.append("        if ((*p & 0xF0U) == 0xE0U) {")
    lines.append("            uint32_t cp = ((uint32_t)(p[0] & 0x0FU) << 12) |")
    lines.append("                          ((uint32_t)(p[1] & 0x3FU) << 6) |")
    lines.append("                          (uint32_t)(p[2] & 0x3FU);")
    lines.append("            if (cp >= 0x4E00U && cp <= 0x9FFFU) {")
    lines.append("                return true;")
    lines.append("            }")
    lines.append("            p += p[1] != '\\0' && p[2] != '\\0' ? 3 : 1;")
    lines.append("        } else {")
    lines.append("            ++p;")
    lines.append("        }")
    lines.append("    }")
    lines.append("    return false;")
    lines.append("}")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def write_h_file(path: Path) -> None:
    path.write_text(
        """#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lvgl.h"

typedef struct {
    const char *text;
    uint8_t size;
    int8_t x_offset;
    int8_t y_offset;
    const lv_img_dsc_t *image;
} ui_text_asset_t;

uint8_t ui_text_asset_normalize_size(uint8_t size);
const ui_text_asset_t *ui_text_asset_find(const char *text, uint8_t size);
bool ui_text_asset_has_cjk(const char *text);
""",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", default="main/ui/display.c", type=Path)
    parser.add_argument("--atlas", default="main/ui/image/text/figma_text_atlas.png", type=Path)
    parser.add_argument("--out-c", default="main/ui/text_assets.c", type=Path)
    parser.add_argument("--out-h", default="main/ui/text_assets.h", type=Path)
    parser.add_argument("--report", default="main/ui/image/text/figma_text_manifest.json", type=Path)
    parser.add_argument("--extra-assets-dir", default="main/ui/image/text/generated_from_figma", type=Path)
    args = parser.parse_args()

    strings = extract_cjk_strings(args.source)
    extra_specs = load_extra_asset_specs(args.extra_assets_dir)
    extra_texts = {spec["text"] for spec in extra_specs}
    atlas_strings = [text for text in strings if text not in extra_texts]
    if args.report.exists():
        previous = json.loads(args.report.read_text(encoding="utf-8"))
        previous_strings = previous.get("atlas_strings") or previous.get("strings") or []
        if previous_strings:
            # Keep the atlas row contract stable. Extra assets are appended and
            # looked up last-to-first, so they can override an existing atlas
            # string without changing atlas geometry.
            atlas_strings = previous_strings
    atlas = Image.open(args.atlas).convert("RGBA")
    assets = build_assets(atlas_strings, atlas)
    assets.extend(build_extra_assets(extra_specs, len(atlas_strings), args.extra_assets_dir))

    args.out_c.parent.mkdir(parents=True, exist_ok=True)
    args.out_h.parent.mkdir(parents=True, exist_ok=True)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    write_c_file(args.out_c, assets)
    write_h_file(args.out_h)
    args.report.write_text(
        json.dumps(
            {
                "source": str(args.source),
                "atlas": str(args.atlas),
                "sizes": list(SIZES),
                "string_count": len(strings),
                "atlas_string_count": len(atlas_strings),
                "asset_count": len(assets),
                "strings": strings,
                "atlas_strings": atlas_strings,
                "extra_assets": [spec["text"] for spec in extra_specs],
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(f"generated {len(assets)} assets from {len(strings)} strings")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
