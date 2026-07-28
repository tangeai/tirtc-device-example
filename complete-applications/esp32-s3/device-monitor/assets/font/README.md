# AI 对讲字库资源

AI 对讲动态字幕只使用一套 18px 字库。当前方案已经从“SPIFFS 外置 binary font + 运行时加载”切换为“编译期 C 字库”：`glyph_bitmap` 作为 `const` 数据编入 app，运行时直接使用 `lv_font_cn_18`，不再读取 `fonts` 分区，也不再创建字体加载任务。

## 目录

- `charset/`: 字符来源，包含 UI 术语、AI 对讲领域词、常用汉字、标点和颜文字符号。
- `generated/`: 合并后的字符表和生成报告。
- `spiffs/`: 旧外置字库目录，默认不再生成、不再参与固件构建。

## 生成方式

在 `main/ui/font` 目录执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\generate_lv_font_cn.ps1
```

默认产物：

- `main/ui/font/lv_font_cn_18.c`
- `assets/font/generated/lv_font_cn_chars.txt`
- `assets/font/generated/lv_font_cn_fallback_chars.txt`
- `assets/font/generated/font_charset_report.txt`

`generate_lv_font_cn.ps1` 默认生成完整 C 字库。只有明确传 `-GenerateSpiffsBinary` 时，才会额外生成旧的 `assets/font/spiffs/lv_font_cn_18.bin`，该模式仅用于对比实验，不是当前发布路径。

## 覆盖范围

默认使用 `simhei.ttf` 生成粗体效果，少量扩展汉字由 Noto Sans SC 补全，并加入常用标点、全角符号、AI 对讲领域词和颜文字符号。18px 字库使用 LVGL RLE 压缩后编入 app，以减少 OTA 分区压力；非 AI 页面的固定中文仍然走 Figma PNG 资产，不再扩大运行时中文字库预算。

## 分区影响

完整 C 字库会显著增大 `sample_project.bin`。因此分区表已经移除旧 `fonts` SPIFFS 分区，并把空间回收到两个 OTA app 分区：

- `ota_0`: `0x10000`, size `0x770000`
- `ota_1`: `0x780000`, size `0x770000`
- `storage`: `0xF00000`, size `0x100000`

分区表变化后，首次刷机需要写入新的 partition table；如果设备上已经刷过旧分区布局，建议执行一次完整烧录或擦除后重刷。
