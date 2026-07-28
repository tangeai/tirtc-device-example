#!/usr/bin/env python3
"""Audit fixed Chinese UI text against the Figma PNG text asset contract."""

from __future__ import annotations

import ast
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DISPLAY_C = ROOT / "main" / "ui" / "display.c"
TEXT_ASSETS_C = ROOT / "main" / "ui" / "text_assets.c"

AI_TEXT_ALLOWED_FUNCTIONS = {
    "display_create_ai_header",
    "display_build_ai_chat_page",
    "display_build_ai_chat_settings_page",
    "display_update_ai_chat_page",
    "display_update_ai_chat_settings_page",
}


def unescape_c_string(literal: str) -> str:
    try:
        return ast.literal_eval(literal)
    except Exception:
        return literal[1:-1]


def has_cjk(text: str) -> bool:
    return re.search(r"[\u4e00-\u9fff]", text) is not None


def line_for(source: str, offset: int) -> int:
    return source.count("\n", 0, offset) + 1


def extract_display_cjk_strings(source: str) -> dict[str, int]:
    strings: dict[str, int] = {}
    for match in re.finditer(r'"(?:\\.|[^"\\])*"', source):
        value = unescape_c_string(match.group(0))
        if has_cjk(value) and value not in strings:
            strings[value] = line_for(source, match.start())
    return strings


def extract_asset_strings(source: str) -> set[str]:
    strings: set[str] = set()
    for match in re.finditer(r'\{"((?:\\.|[^"\\])*)",\s*(?:10|12|16),', source):
        strings.add(unescape_c_string(f'"{match.group(1)}"'))
    return strings


def function_ranges(source: str) -> list[tuple[int, str]]:
    functions: list[tuple[int, str]] = []
    pattern = re.compile(
        r"^static\s+[\w\s\*]+?(display_[A-Za-z0-9_]+)\s*\([^;]*?\)\s*\{",
        re.MULTILINE | re.DOTALL,
    )
    for match in pattern.finditer(source):
        functions.append((match.start(), match.group(1)))
    functions.sort()
    return functions


def function_for_offset(functions: list[tuple[int, str]], offset: int) -> str:
    current = "<global>"
    for start, name in functions:
        if start > offset:
            break
        current = name
    return current


def call_chunk(source: str, offset: int) -> str:
    end = source.find(";", offset)
    if end < 0:
        end = min(len(source), offset + 500)
    return source[offset:end]


def cjk_literals_in_chunk(chunk: str) -> list[str]:
    values: list[str] = []
    for match in re.finditer(r'"(?:\\.|[^"\\])*"', chunk):
        value = unescape_c_string(match.group(0))
        if has_cjk(value):
            values.append(value)
    return values


def audit_ai_text_usage(source: str) -> list[str]:
    functions = function_ranges(source)
    errors: list[str] = []
    for match in re.finditer(r"display_create_ai_text\s*\(", source):
        values = cjk_literals_in_chunk(call_chunk(source, match.start()))
        if not values:
            continue
        function = function_for_offset(functions, match.start())
        if function not in AI_TEXT_ALLOWED_FUNCTIONS:
            errors.append(
                f"{DISPLAY_C}:{line_for(source, match.start())}: "
                f"display_create_ai_text carries CJK outside AI UI: {function}: {values!r}"
            )
    return errors


def audit_direct_label_cjk(source: str) -> list[str]:
    functions = function_ranges(source)
    errors: list[str] = []
    for match in re.finditer(r"lv_label_set_text(?:_fmt)?\s*\(", source):
        values = cjk_literals_in_chunk(call_chunk(source, match.start()))
        function = function_for_offset(functions, match.start())
        if values and function not in AI_TEXT_ALLOWED_FUNCTIONS:
            errors.append(
                f"{DISPLAY_C}:{line_for(source, match.start())}: "
                f"direct label CJK must use display_create_figma_text/display_text_set: {values!r}"
            )
    return errors


def main() -> int:
    display_source = DISPLAY_C.read_text(encoding="utf-8")
    asset_source = TEXT_ASSETS_C.read_text(encoding="utf-8")

    display_strings = extract_display_cjk_strings(display_source)
    asset_strings = extract_asset_strings(asset_source)

    errors: list[str] = []
    for text, line in sorted(display_strings.items(), key=lambda item: item[1]):
        if text not in asset_strings:
            errors.append(f"{DISPLAY_C}:{line}: missing Figma PNG text asset: {text!r}")

    errors.extend(audit_ai_text_usage(display_source))
    errors.extend(audit_direct_label_cjk(display_source))

    if errors:
        print("UI text asset audit failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(
        "UI text asset audit passed: "
        f"{len(display_strings)} CJK strings covered by {len(asset_strings)} PNG text assets."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
