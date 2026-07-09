# tirtc-device-example

`tirtc-device-example` 是面向多芯片平台的 TiRTC 设备端示例集合，用来沉淀可编译、可烧录、可体验的设备侧参考工程。

## 当前示例

| 平台 | 示例目录 | 示例版本 | TiRTC SDK |
| --- | --- | --- | --- |
| ESP32-S3 | `esp32-s3/trtc-advanced` | `0.7.5` | `2.2.0` |
| ESP32-P4 | `esp32-p4/trtc-advanced` | `1.0.2` | `2.2.0` |

## 目录约定

- `esp32-s3/`：ESP32-S3 平台示例。
- `esp32-p4/`：ESP32-P4 平台示例。
- `common/`：跨平台公共资料预留目录。
- `docs/`：统一仓公共文档预留目录。

S3 和 P4 是同级平台目录。发布资产分别放在各自示例目录下的 `release_assets/`，不要跨平台混放。
