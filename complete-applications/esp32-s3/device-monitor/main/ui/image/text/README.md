# Figma text image assets

Static Chinese UI text is rendered from Figma-exported PNG masks instead of the
runtime Chinese font. The only remaining Chinese font is reserved for AI chat
dynamic text.

Regeneration flow:

1. Extract current fixed UI strings from `main/ui/display.c`.
2. Render them in Figma as white text on a transparent atlas.
3. Save the atlas to `main/ui/image/text/figma_text_atlas.png`.
4. Run `python tools/generate_figma_text_assets.py`.

The generator emits `main/ui/text_assets.c`, `main/ui/text_assets.h`, and
`main/ui/image/text/figma_text_manifest.json`. Runtime color comes from LVGL
image recolor, so each text string is stored once per size.

Before committing UI text changes, run:

```powershell
python tools/audit_ui_text_assets.py
```

The audit checks that every fixed Chinese string used by `main/ui/display.c`
has a PNG text asset, and that `display_create_ai_text` is not reused for
non-AI Chinese UI text.

Single text assets exported from Figma can be placed in
`main/ui/image/text/generated_from_figma` and listed in
`extra_text_assets.json`. They are merged after the atlas so the runtime still
uses the same `text_assets.c` lookup path.
