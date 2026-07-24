# ESP32-S3 trtc-advanced v0.7.5

## 版本定位

`0.7.5` 把 ESP32-S3 设备端示例对齐到 ThingConnect 当前设备协议，并补齐 H5 实时查看、
AI 对讲、微信 IoT VoIP 和设备间互呼共用一套硬件与 TiRTC 资源时的生命周期边界。

设备协议、接口字段和错误码以
[tangeai/tirtc-server-example](https://github.com/tangeai/tirtc-server-example)
为准；本仓库提供对应的 ESP-IDF 设备实现。

## 主要变化

- 增加 ThingConnect 服务发现和统一服务注册表，设备、MQTT、TiRTC、AI、VoIP 和 call
  服务地址由同一入口解析，编译期地址只作为失败兜底。
- 收口首次绑定、保留身份重绑、云端解绑、HTTP 410/业务码 6006 和正式 MQTT 恢复流程。
- 正式 MQTT 支持分片消息重组、ACK、心跳、自动重连和按 `channel` 分发，避免微信 VoIP
  与设备互呼消费同一条来电消息。
- 微信联系人切换到正式接口 `GET /v1/voip/device/contacts`，保留服务端旧地址兼容能力，
  设备端不调用需要用户 JWT 的 `/v1/voip/user/*` 接口。
- 增加完整设备互呼业务：云端联系人、申请/接受/备注、主被叫房间、接听、拒接、取消、
  挂断、`0x2000/0x2001` 命令和异常房间恢复。
- 增加 H5、AI、微信 VoIP、设备互呼的统一会话仲裁。UI 和协议回调只投递事件，资源切换、
  RTC 断开、音频和摄像头释放由应用层串行处理。
- 优化主界面点击翻页、设备二维码、联系人页、呼叫页和屏幕调试服务。
- 补充与服务端同结构的首次体验、功能矩阵、协议链接、烧录、OTA 和问题排查文档。
- 固件 `.bin/.zip` 迁出 Git 历史，正式下载改用 GitHub Releases。

## 构建契约

| 项目 | 值 |
| --- | --- |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.2.0` |
| TiRTC BuildInfo commit | `1df9e045a9dc` |
| TiRTC 静态库 SHA-256 | `0686E5D7F5BFEA18B7B3EE6AE1701061B3AFCFB0EF2030642EBEBAB88AF0413D` |
| FreeRTOS tick | `1000 Hz` |
| Flash | `16 MB` |
| PSRAM | `8 MB` |

## Release 资产

下载页：
[esp32-s3-trtc-advanced-v0.7.5](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-trtc-advanced-v0.7.5)

| 资产 | 大小 | SHA-256 |
| --- | ---: | --- |
| `esp32s3-tirtc-device-monitor-ota-v0.7.5.bin` | `7291120` | `E813347914D70D284D82E3AB798E54A6292530CE97A20C30FE4EBA58FE32BFFC` |
| `esp32s3-tirtc-device-monitor-full-v0.7.5.bin` | `16777216` | `81AA9FE1E1CC0E8A6F428A1D675BE78DA56558F09BD5C45CAD819C46ADA95B46` |
| `esp32s3-tirtc-device-monitor-webinstall-v0.7.5.zip` | `5487666` | `83473E73399F5080FAEDEA605A745A73B3F8F52D759EC1FC0EF5511323DE7F79` |
| `esp32s3-tirtc-device-monitor-webflash-v0.7.5.zip` | `5477614` | `8C6769BDA7BB0EC3BF950F7C4F73B3901C4DA2C6892DD5C7F26D993C52076534` |

普通体验者使用 `full` 完整镜像，ESP Launchpad 地址填写 `0x0`。`ota` 文件只供 OTA
服务端使用。

## 验证记录

| 检查项 | 结果 |
| --- | --- |
| `idf.py build` | 通过 |
| app 分区 | `0x6f40f0 / 0x770000`，剩余 `0x7bf10`，约 7% |
| `build/project_description.json` | `project_version=0.7.5` |
| Release 资产 SHA-256 | 已生成并写入 `SHA256SUMS.txt` |
| 敏感信息扫描 | 发布前执行 |
| ESP Launchpad 实机烧录 | 待真机验证 |
| 上一版本到 0.7.5 OTA | 待真机验证 |
| H5 / AI / 微信 VoIP / 设备互呼 | 本轮源码改动待真机回归 |

构建通过不等于真机闭环。未执行的项目保持“待验证”，不写成“已验证”。
