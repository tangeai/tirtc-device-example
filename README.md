# TiRTC 设备端示例

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-000000?logo=espressif)](esp32-s3/trtc-advanced)
[![ESP32-P4](https://img.shields.io/badge/Platform-ESP32--P4-E7352C?logo=espressif)](esp32-p4/trtc-advanced)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.2.0-1769AA)](https://docs.tange.ai/products/tirtc/overview/what-is-tirtc.html)

面向 MCU 和嵌入式设备的 TiRTC 示例合集。仓库中的 S3、P4 示例与
[tangeai/tirtc-server-example](https://github.com/tangeai/tirtc-server-example)
使用同一套 ThingConnect 设备协议，演示真实设备如何完成**联网、6 位验证码绑定、
H5 实时查看与对讲、AI 对讲、微信 IoT VoIP、设备间互呼和 OTA**。

固件 `.bin/.zip` 不提交到 Git 历史。普通体验者从
[GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)
下载对应开发板的完整镜像，开发者再进入平台目录阅读源码和构建说明。

---

## 首次体验：让开发板在 H5 中出图、出声

第一次使用只走这一条主线：

```text
选择开发板 -> 下载完整镜像 -> 0x0 烧录 -> 连接 2.4 GHz Wi-Fi
             -> 输入 6 位验证码 -> H5 查看音视频
```

完成后，开发板、网络、设备身份、MQTT、TiRTC 和音视频驱动已经端到端工作。AI、
微信 VoIP 和设备互呼应在这条链路通过后逐项体验。

### 开始前

- 一块本仓支持的开发板，见下表。
- 一根可以传输数据的 USB 线。只供电的线无法烧录。
- 2.4 GHz Wi-Fi。
- Chrome 或 Edge 浏览器。
- 一个可登录 [ThingConnect H5](https://mqtt-demo.tange-ai.com/) 的账号。

### 步骤 1：选择开发板和固件

| 平台 | 已验证开发板 | 示例版本 | 完整镜像下载 | 平台说明 |
| --- | --- | --- | --- | --- |
| ESP32-S3 | 嘉立创/立创·实战派 `LCKFB-SZPI-ESP32-S3-VA` | `0.7.5` | [S3 Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-trtc-advanced-v0.7.5) | [S3 README](esp32-s3/trtc-advanced/README.md) |
| ESP32-P4 | Waveshare `ESP32-P4-WIFI6-Touch-LCD-3.5` | `1.1.0` | [P4 Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-trtc-advanced-v1.1.0) | [P4 README](esp32-p4/trtc-advanced/README.md) |

在对应 Release 中下载名称含 `full` 的 16 MiB 完整镜像。不要只下载 OTA app：

```text
ESP32-S3: esp32s3-tirtc-device-monitor-full-v0.7.5.bin
ESP32-P4: tirtc-esp32p4-device-monitor-full-v1.1.0.bin
```

**完成标志：** 下载文件名与开发板平台一致，文件大小为 `16777216` 字节。S3 固件不能
烧到 P4，P4 固件也不能烧到 S3。

### 步骤 2：使用 Espressif 官方网页烧录

1. 用 USB 数据线连接开发板和电脑。
2. 用 Chrome 或 Edge 打开 [Espressif ESP Launchpad](https://espressif.github.io/esp-launchpad/)。
3. 进入 DIY 烧录模式并选择开发板对应的串口。
4. 添加刚下载的 `full` 完整镜像，烧录地址填写 `0x0`。
5. 第一次烧录或设备状态不确定时，先执行“擦除闪存”。
6. 开始烧录，等待网页明确显示完成。
7. 按一下开发板 RESET，等待屏幕进入主页。

如果浏览器找不到串口，先更换确认能传数据的 USB 线。仍然失败时，按住 BOOT，
点一下 RESET，松开 RESET 后再松开 BOOT，然后重新连接。

**完成标志：** 网页明确显示烧录完成；按 RESET 后设备可以进入主页，并在设置页看到
与所下载 Release 一致的版本号。

### 步骤 3：连接 Wi-Fi

1. 在设备主页进入“设置” -> “Wi-Fi 设置”。
2. 选择 2.4 GHz Wi-Fi，输入密码并点击连接。
3. 返回主页，确认顶部 Wi-Fi 图标已显示真实信号状态。

连接成功后，SSID 和密码会保存到 NVS，下次开机自动连接。

**完成标志：** 顶部 Wi-Fi 图标显示真实信号，设置页能看到当前 SSID 和 IP；重启后
设备可以自动重连。

### 步骤 4：用 6 位验证码绑定设备

设备首次联网后会显示 6 位验证码。打开
[ThingConnect H5](https://mqtt-demo.tange-ai.com/)，注册或登录账号，在设备页面输入
验证码。绑定成功后，设备自动保存身份并建立正式 MQTT 和 TiRTC 链路。

普通体验者不需要手动填写 `device_id`、`device_key` 或设备密钥，也不要把密钥放进
截图、文档或公开仓库。

**完成标志：** H5 设备列表出现当前设备，设备主页显示正式设备 ID，不再停留在等待
绑定状态。

### 步骤 5：查看实时音视频

1. 在 H5 设备列表中找到刚绑定的设备。
2. 点击设备进入实时查看页。
3. 确认能看到设备摄像头画面，并听到设备麦克风音频。
4. 使用 H5 对讲按钮，确认设备扬声器能播放网页端声音。
5. 退出实时查看，确认设备回到待机状态。

至此，首个闭环完成。

### 第一次失败时怎么查

先停在失败的那一步，不要同时改 Wi-Fi、绑定和 TiRTC 配置。

| 现象 | 先检查 |
| --- | --- |
| 浏览器没有串口 | USB 线是否支持数据、串口是否被串口工具占用、开发板是否进入下载模式 |
| 烧录后还是旧版本 | 是否选中了当前平台的 `full` 文件、地址是否为 `0x0`、烧录后是否按 RESET |
| Wi-Fi 连不上 | 是否为 2.4 GHz、密码是否正确、路由器是否限制新设备接入 |
| 不显示 6 位验证码 | 设备是否拿到 IP、系统时间是否同步、设备是否已经绑定 |
| H5 找不到设备 | H5 登录账号是否正确、绑定是否完成、设备是否保持在线 |
| 有画面但没有声音 | 浏览器是否允许播放音频、设备音量是否为 0、H5 是否已订阅音频 |

仍未解决时，再打开对应平台 README 和串口日志定位。不要把设备密钥、token 或完整
授权响应贴到公开 issue。

---

## 下一步：逐项体验四类能力

设备端功能和服务端文档按同一业务边界组织。字段、接口、错误码和状态机以右侧
`tirtc-server-example` 文档为协议依据。

| 能力 | 设备端完成标志 | 服务端协议文档 |
| --- | --- | --- |
| H5 实时查看与对讲 | H5 出图、出声，按住说话后设备扬声器播放 | [H5 实时查看与按住说话](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-h5-live.md) |
| AI 对讲 | 麦克风上行，收到字幕和 AI 语音，退出后资源释放 | [AI 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-ai.md) |
| 微信 IoT VoIP | 小程序授权后可双向呼叫、接听和挂断 | [微信 VoIP 对讲设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-voip.md) |
| 设备间互呼 | 联系人可见，主被叫双向音视频，结束后房间释放 | [设备呼设备接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-call.md) |

推荐按以下顺序验证：

1. H5 实时查看与对讲。
2. AI 对讲。
3. 微信 IoT VoIP。
4. 设备间互呼。
5. OTA 升级和回滚验证。

---

## 开发者入口

### 仓库结构

```text
tirtc-device-example/
├── esp32-s3/
│   └── trtc-advanced/      ESP32-S3 触摸屏设备完整示例
├── esp32-p4/
│   └── trtc-advanced/      ESP32-P4 触摸屏设备完整示例
├── common/                 跨平台公共资料
└── docs/                   仓库级说明
```

S3 和 P4 是同级平台目录。每个平台独立维护板级驱动、构建配置、版本号和发布标签，
不互相复制构建产物。

### 源码构建

| 平台 | 构建入口 |
| --- | --- |
| ESP32-S3 | [S3 构建说明](esp32-s3/trtc-advanced/README.md#源码构建) |
| ESP32-P4 | [P4 构建说明](esp32-p4/trtc-advanced/README.md#源码构建) |

构建成功只证明源码编译和链接通过，不能代替 Wi-Fi、绑定、H5、AI、微信 VoIP、
设备互呼和 OTA 的真机验证。

### 协议和架构

- [设备上线与 MQTT 接入](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-integration.md)
- [统一会话模型](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/device-session-model.md)
- [ThingConnect API Reference](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/api-reference.md)
- [仓库文档入口](docs/README_CN.md)

---

## 固件发布规则

- Git 只管理源码、文档、脚本和构建所需的 SDK 静态库。
- `.bin/.zip` 不进入 Git 历史。
- 正式人工烧录包发布到 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases)。
- OTA app 和 manifest 由 OTA 服务保存；GitHub Release 用于下载、归档和人工烧录。
- GitHub Actions Artifact 只用于临时构建检查，不作为长期下载地址。
- 每个 Release 必须带 `SHA256SUMS.txt` 和 `release-manifest.json`。
- 标签按平台隔离，例如 `esp32-s3-trtc-advanced-v0.7.5`。

详细规则见 [发布与二进制管理](docs/RELEASES_CN.md)。

## License

MIT © 探鸽智能
