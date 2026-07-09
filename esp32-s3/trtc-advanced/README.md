# TiRTC ESP32-S3 Device Monitor Demo

这是一个面向 ESP32-S3 设备端的 TiRTC 示例工程，用来展示设备联网、TiRTC 音视频通信、微信通话、AI 对讲、OTA 和基础设备管理能力。

后续公开远端仓库统一使用 [tangeai/tirtc-device-example](https://github.com/tangeai/tirtc-device-example)。当前 ESP32-S3 设备监控示例会作为 `esp32-s3/trtc-advanced` 进入该仓库；`C:\Users\61852\Desktop\tirtc_esp32s3_wifi_link_demo` 后续仅作为本地测试和历史验证目录使用。仓库目录规划见 [公开仓库目录规划](docs/release/PUBLIC_REPO_LAYOUT_CN.md)。

## 能体验什么

- 设备开机后进入触摸屏主页，主页包含查看、呼叫、微信、AI 对讲和设置入口。
- 通过设置页连接 Wi-Fi，连接成功后设备会保存 SSID 和密码，后续启动会自动连接。
- 在查看页确认设备网络状态、IP 和基础运行状态。
- IPC/设备监控功能在 Web 端体验，正式体验地址为 <https://demo-tirtc.tange365.com/>。
- 在呼叫页体验 TiRTC 设备间音视频通话，支持添加联系人、发起呼叫、接听、挂断、调节接收音量和发送音量。
- 在微信页配合微信小程序体验联系人绑定、VoIP 授权和微信通话流程。
- 在 AI 对讲页体验设备端语音上行、云端 AI 回复、字幕展示和语音播放。
- 在系统设置页进行网络测试、TiRTC 配置、TiRTC 测试和 OTA 相关操作。

## 快速开始

推荐先按这份教程走完整体验链路：

- [完整体验流程：从开机到 IPC、微信、AI 和 OTA](docs/USER_EXPERIENCE_FLOW_CN.md)
- [从连接 Wi-Fi 到体验功能](docs/GETTING_STARTED_CN.md)
- [烧录与 OTA 快速教程](docs/FLASH_AND_OTA_CN.md)

体验本工程请购买嘉立创/立创开发板的“立创·实战派 ESP32-S3 开发板”，型号可按 `LCKFB-SZPI-ESP32-S3-VA` 查找。本工程里的 `esp32s3_tirtc_monitor` 就是按这块板子的屏幕、触摸、音频和摄像头硬件适配的。购买入口可参考立创开发板项目页 <https://lckfb.com/project/detail/lckfb-esp32-s3-va?param=baseInfo> 或立创商城 <https://item.szlcsc.com/43285221.html>。普通 ESP32-S3 DevKit 不能完整替代这块板。

首次体验不需要手动准备或公开填写设备密钥。设备连接 Wi-Fi 后，如果本地还没有绑定凭证，主页会自动弹出设备绑定窗口，显示 6 位绑定码和绑定入口。体验者在绑定网页、业务后台或配套小程序里输入这 6 位验证码，设备会通过业务服务拿到自己的 `device_id/device_key` 并保存到 NVS。

绑定完成前，Web IPC、设备呼叫、微信通话和 AI 对讲的授权链路不会完整闭环。完整流程见 [从连接 Wi-Fi 到体验功能](docs/GETTING_STARTED_CN.md#3-用-6-位验证码完成设备绑定)。

Web 端 IPC 体验入口：

- 正式域名：<https://demo-tirtc.tange365.com/>
- 设备侧先连接 Wi-Fi，并完成 6 位验证码绑定。
- 进入主页的查看页面，确认设备已上线并显示当前设备 ID。
- 浏览器打开 Web Demo，选择 IPC/设备监控入口，按页面提示输入或扫码导入设备信息后连接。

微信通话体验入口：

- 微信端使用 TiRTC 微信 VoIP 示例小程序。体验版小程序二维码由项目维护者从微信公众平台生成并提供，用户用微信扫码进入小程序。
- 设备侧使用 thing-connect 业务链路：6 位验证码绑定成功后保存 `device_id` / `device_key`，再通过 `APP_CONFIG_DEVICE_BINDING_API_BASE` 获取 MQTT token，连接 `APP_CONFIG_DEVICE_BINDING_MQTT_URI`。微信 VoIP 只在同一个 MQTT 通道上订阅来电、取消和联系人更新消息。
- 当前默认业务 HTTP 服务为 `http://mqtt-demo.tange-ai.com`，默认 MQTT 地址为 `mqtts://mqtt-demo.tange-ai.com:8883/mqtt`。
- 小程序 AppID 示例为 `wx27d4b2d7eb37eb58`，微信 IoT VoIP ModelID 示例为 `HRHY_vJ9mHI2KQhd6yvj9Q`。如果换成自己的小程序和服务端，需要同步替换 AppID、ModelID、业务服务配置和设备侧绑定服务配置。
- 注意区分两个二维码：小程序二维码用于打开微信端体验；设备 ID 二维码只用于把 12 位 `device_id` 加入小程序联系人，不是小程序入口。
- 不要把 `mp.weixin.qq.com/wxamp/home/guide?...token=...` 这类后台地址当作体验入口分发，它只是微信公众平台的管理页面地址。

开发者常用构建命令：

```powershell
. 'C:\esp\v5.5.4\esp-idf\export.ps1'
idf.py build
```

开发者命令行烧录和串口验证请按实际开发板端口执行：

```powershell
idf.py -p COMx flash monitor
```

给普通体验者分发固件时，统一使用 Espressif 官方 ESP Launchpad：<https://espressif.github.io/esp-launchpad/>。正式体验包需要从当前源码重新构建并生成 `0x0` 完整合一镜像，烧录地址填 `0x0`；具体地址和操作步骤见 [烧录与 OTA 快速教程](docs/FLASH_AND_OTA_CN.md)。

## 主要目录

| 目录 | 说明 |
| --- | --- |
| `main/ui` | LVGL 屏幕界面、主页、设置页、Wi-Fi 页面、通话页面、AI 对讲页面 |
| `main/application` | 应用生命周期、页面进入退出、资源申请释放、Wi-Fi/RTC/音频策略 |
| `main/connectivity` | 网络抽象层和 Wi-Fi STA 驱动封装 |
| `main/drivers` | 屏幕、触摸、音频、摄像头和虚拟媒体驱动 |
| `main/protocols` | RTC 抽象层和 TiRTC 协议实现 |
| `main/services` | 设备信息、媒体桥、AI 对讲、微信通话、OTA 等业务服务 |
| `docs` | 架构说明、AI 对讲说明、发布和上手文档 |

## 配置提醒

发布代码不内置默认 Wi-Fi、设备号、TiRTC 凭证或 AI Chat 服务端密钥。普通体验流程通过 6 位验证码绑定写入设备身份；开发调试可以使用私有配置或后台下发方案。不要把真实设备密钥、长期有效服务端密钥或个人联系人标识提交到公开仓库。
