# TiRTC ESP32-S3 Device Monitor

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-E7352C?logo=espressif)](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/)
[![TiRTC SDK](https://img.shields.io/badge/TiRTC%20SDK-2.3.0%20mini-1769AA)](https://docs.tange.ai/products/tirtc/en/overview/what-is-tirtc.html)
[![Board](https://img.shields.io/badge/Board-LCKFB--SZPI--ESP32--S3--VA-2E7D32)](https://lckfb.com/project/detail/lckfb-esp32-s3-va?param=baseInfo)

这是面向立创·实战派 ESP32-S3 开发板的完整设备应用。它把 2.4 GHz Wi-Fi、ThingConnect
绑定与在线、TiRTC 实时音频、320 x 240 触摸屏、小钛、微信 VoIP、设备互呼和 OTA 放在
同一套应用生命周期里，适合直接体验，也适合作为带屏音频设备的工程参考。

当前版本为 `1.9.7`，公开项目 Tag 为 `esp32-s3-device-monitor-v1.9.7`，TiRTC SDK 为
`2.3.0 mini`。

> **1.9.7 产品边界**
>
> 本版本的 RTC 业务统一为双向音频：Web IPC、微信 VoIP、设备互呼和小钛发起的呼叫都不
> 发布或播放 RTC 视频。板载摄像头只用于扫描联系人二维码。这样可以让 UI、业务和协议层
> 对“这台设备能做什么”保持同一个答案。

## 先选你的路线

| 目标 | 最短入口 |
| --- | --- |
| 直接体验 | 下载完整镜像，使用 Espressif ESP Tool 从 `0x0` 烧录 |
| 修改源码 | 安装 ESP-IDF `5.5.4`，从公开 Tag 干净构建 |
| 接入自己的平台 | 先读 [ThingConnect 接入与实现对齐](docs/THING_CONNECT_ALIGNMENT_CN.md) |
| 排查音频或弱网 | 使用默认开启的串口诊断 CLI，并阅读 [音频和媒体所有权](docs/architecture/audio_media_ownership.md) |

## 直接烧录

从
[`esp32-s3-device-monitor-v1.9.7` Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-s3-device-monitor-v1.9.7)
下载：

```text
esp32s3-tirtc-device-monitor-full-v1.9.7.bin
SHA256SUMS.txt
release-manifest.json
```

先核对 SHA-256，再用 Chrome 或 Edge 打开
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，选择完整镜像并把烧录地址填写为
`0x0`。完整镜像会覆盖 16 MB Flash，并清除 NVS 中已有的 Wi-Fi、绑定和本地设置。

详细操作、OTA 文件区别和多地址烧录见 [烧录与 OTA](docs/FLASH_AND_OTA_CN.md)。

## 硬件要求

| 项目 | 要求 |
| --- | --- |
| 芯片 | ESP32-S3，双核 240 MHz |
| 开发板 | `LCKFB-SZPI-ESP32-S3-VA` |
| Flash / PSRAM | 16 MB / 8 MB |
| 显示 | 320 x 240 触摸屏 |
| 音频 | 板载麦克风、扬声器和音频 Codec |
| 摄像头 | 仅用于二维码扫描，不作为 RTC 视频源 |
| 网络 | 2.4 GHz Wi-Fi，可访问服务发现、ThingConnect 和 TiRTC 服务 |

普通 ESP32-S3 DevKit 缺少这块板的屏幕、触摸和音频外设，不能直接复现完整体验。

## 第一次启动

```text
烧录完整镜像
  -> 在屏幕上连接 2.4 GHz Wi-Fi
  -> 获取 6 位绑定码
  -> 在 ThingConnect H5 完成绑定
  -> 验证 Web IPC 双向音频
  -> 再体验小钛、微信 VoIP、设备互呼和 OTA
```

判断每一步时看真实现象：

1. 设置页显示版本 `1.9.7`。
2. Wi-Fi 页面显示已连接、IP 地址和信号状态。
3. 设备显示 6 位验证码，绑定后显示正式设备 ID。
4. H5 能听到设备麦克风音频，按住说话时设备扬声器能播放回传音频。
5. 小钛、微信和设备互呼分别完成进入、通话与退出，退出后可以正常进入下一项业务。

完整操作见 [从 Wi-Fi 到体验功能](docs/GETTING_STARTED_CN.md)。

## 当前能力

| 能力 | 设备端行为 |
| --- | --- |
| 设备接入 | HTTPS 服务发现、6 位码绑定、设备身份保存、MQTTS 在线和自动重连 |
| Web IPC | 设备麦克风上行、H5 音频下行；不启用 RTC 视频 |
| 小钛 | WHIP 音频、字幕、打断、联系人状态查询和音频呼叫动作 |
| 微信 VoIP | 联系人、备注、设备与微信双向发起的音频通话；主动呼叫使用体验版 `wx_version_type=2` |
| 网络测试 | 显示平均时延、相邻 RTT 变化的平均抖动和丢包率，并把同一组结构化结果传到 UI 与串口诊断 |
| 设备互呼 | 音频呼叫、来电铃声、接听、拒接、取消、挂断和异常房间恢复 |
| 摄像头 | 扫描设备联系人和微信联系人二维码 |
| OTA | 双 OTA app 分区，在线更新仅替换 app |
| 诊断 | 串口 AT 命令查看网络、Socket、RTC、音频路径、AEC、媒体统计和呼叫状态 |

多项 RTC 业务共享音频设备和 TiRTC 连接资源。应用层负责会话仲裁与资源交接；MQTT 和设备
在线属于常驻服务，不随单个页面销毁。

## 网络与 SDK 安全

设备使用 `https://ep-open.tangeopen.com/services` 获取业务地址，只接受 `HTTPS` 和
`MQTTS` 结果。发现响应或本地覆盖若给出 `http://`、`mqtt://`，应用会拒绝并报错，同时保留
安全兜底，不会把 Token、设备密钥或控制消息降级到明文传输。

应用 HTTP 和 MQTT 客户端使用 ESP-IDF 证书包。TiRTC SDK 自有的 HTTPS 客户端也校验证书链
与 hostname，握手或证书校验失败时返回原有 SSL 错误路径，不回退 HTTP。公开静态库在完成
上述修复后执行 `--strip-debug`，只去除 SDK 内部源码路径和行号级调试信息；成员、符号、代码
和只读数据契约保持一致。详细版本与哈希见
[`components/tirtc_sdk/VERSION.md`](components/tirtc_sdk/VERSION.md)。

## 1.9.7 的呼叫与交互收口

- 接听、拒绝、挂断不再在 LVGL 回调中直接执行网络或 RTC 操作。UI 只投递动作，应用任务按
  顺序完成业务处理，让按键反馈和后台状态各自保持清楚。
- 扬声器音量与采集增益也由应用任务处理。用户连续点击时，中间值可以合并，只应用最新值，
  避免为了追赶每一次点击而堆积控制任务。
- 每次呼叫都有独立 generation。旧的接听 worker 会在提交连接前、提交过程中和等待连接时
  复核 generation；呼叫已经结束后，它不能再把旧房间拉回活动状态。
- 主叫先等待对端接听，响铃上限为 `45` 秒；进入 `CONNECTING` 后重新开始 `40` 秒 P2P
  建连计时。两个阶段分别上报取消或挂断，不再共用一只模糊的总计时器。
- 来电铃声改为 `659 / 784 / 1047 Hz` 的两组三音上行短句，周期 `3.2` 秒，并带起音和收音
  包络。接听、拒绝、取消、挂断或身份重置仍会停止铃声。
- 接听、拒绝和挂断按钮在按下时立即投递动作；这改善触摸反馈，但不代表动作已在云端或
  P2P 侧完成，最终结果仍以页面状态和日志为准。
- TiRTC SDK 的 ICE UDP 接收与 TGTRP 音频抖动队列改为有界批处理，让一次网络回调不会长期
  占住 RTC 调度线程；正常单帧流量不增加固定延时。

## 持续保留的音频契约

- Web IPC、设备互呼和微信 VoIP 的设备上行线格式均核对为 `8 kHz / 16 bit / mono /`
  `G.711 A-law`，每包 `20 ms / 160 bytes`；设备互呼的显式 CALL 发送路径使用同一契约。
- 设备互呼单独使用有界自适应播放缓冲。缓冲水位偏低或偏高时，每个 20 ms 播放周期只
  调整 4 个 16 kHz 采样帧，约为 `1.25%`，其余 Web IPC、微信和小钛配置不跟随改变。
- 设备互呼 AEC 使用全双工高性能线性模式，工作区放在 PSRAM；AEC 后增加 100 Hz 高通，
  并提高通话 AGC 的静态噪声底线，避免把已测得的板端静态残留继续放大。
- 普通设备来电会播放本地合成铃声；两组三音短句使用 `659 / 784 / 1047 Hz`，每 `3.2` 秒
  重复，并带起音和收音包络。接听、拒接、对端取消、挂断或身份重置时按状态停止。铃声任务栈
  和 PCM 缓冲使用 PSRAM，不把大块临时数据挤进 internal RAM。

> **已知音频现象**
>
> Web IPC 和设备互呼的当前人耳试听仍可感知轻微“沙沙电流声”。线上格式核对和 20,000 次
> A-law 编解码自检均未发现异常，这些证据缩小了排查范围，但底噪根因尚未证实。本版本不
> 宣称该问题已经解决；排查时请保留业务类型、设备角色、音频路径和主观听感记录。

## 串口诊断 CLI

`CONFIG_APP_SERIAL_NET_CLI_ENABLE` 在公开默认配置中开启。连接 ESP-IDF Monitor 后输入
`AT+HELP` 可查看当前固件实际支持的命令，常用入口包括：

```text
AT+NET?       AT+WIFI?       AT+NETPROBE
AT+SOCKETS?   AT+MEDIA?      AT+AUDIOPATH?
AT+RTCLOG?    AT+RTCLINK?    AT+AUDIOCHECK?
AT+AECDUMP?   AT+CALL?       AT+HEAP?
```

CLI 使用固定缓冲区，不输出 Wi-Fi 密码、Token 或设备密钥。产品量产不需要串口诊断时，可在
`idf.py menuconfig` 的 `TiRTC Device Monitor -> Debug utilities` 中关闭
`APP_SERIAL_NET_CLI_ENABLE`，然后重新做完整干净构建。

## 从源码构建

```powershell
git clone https://github.com/tangeai/tirtc-device-example.git
cd tirtc-device-example
git checkout esp32-s3-device-monitor-v1.9.7
cd complete-applications/esp32-s3/device-monitor

. "$env:IDF_PATH\export.ps1"
idf.py -B build --no-ccache reconfigure build
```

版本和依赖必须同时满足：

| 项目 | 值 |
| --- | --- |
| 应用版本 | `1.9.7` |
| ESP-IDF | `5.5.4` |
| Xtensa 工具链 | `14.2.0_20260121` |
| TiRTC SDK | `2.3.0 mini` 基线；active-connect `db7290f`；HTTPS 认证 `13e34c3` |
| TiRTC BuildInfo | `v2.3.0-db7290f`（BuildInfo 保留基础功能提交身份） |
| `libTiRTC.a` | `2,127,226` bytes |
| `libTiRTC.a` SHA-256 | `f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b` |
| OTA app 分区 | `0x770000` bytes |

来源侧 `1.9.7` 的开发记录不作为公开固件资产。统一发布已从隔离候选的全新检出完成一次
ESP-IDF `5.5.4`、禁用 ccache 的正式干净构建。该候选来自公开代码提交
`a3c6d4c5bdecb215f8c8ebec554424de77ed193b` 的 repository tree
`7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a`、项目 tree
`ac13d559aae8f74e92afbf585fe79c5da02aa324`。结果如下：

| 正式构建项 | 值 |
| --- | --- |
| 构建步骤 | `1767/1767`；compiler warning/error/ICE 均为 `0` |
| app 大小 | `7,613,216` bytes（`0x742b20`） |
| app SHA-256 | `49349c1e1755ae5265e3ddea6f57166ccf60b90cdca7d1a1a004539f4faf43ff` |
| app 分区 | `7798784` bytes |
| 分区剩余 | `185,568` bytes（`2.38%`） |

正式构建确认 app 余量为 `185,568` bytes（`2.38%`），容量已进入需要逐版核对的风险区。
本次没有复用 `1.9.6` 的大小、哈希或固件；继续增加图片、字体、日志或调试能力前，要先
确认新 app 没有越过 OTA 分区。

构建输入、配置和多地址烧录见 [源码构建与配置](docs/BUILD_AND_CONFIG_CN.md)。

## 版本来源

| 项目 | 值 |
| --- | --- |
| 开发来源 Tag | `v1.9.7`（Tag object `73f6a3b18ac4ad5e95f813c6eba4f31907961be6`） |
| 开发来源 commit | `58c2d152a179fa382511f4217fb84858072ffbb0` |
| 开发来源 tree | `206b2373a389fab39ece1893f23f1ddbf0e66dfe` |
| 公开代码 commit | `a3c6d4c5bdecb215f8c8ebec554424de77ed193b` |
| 公开 repository tree | `7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a` |
| 公开项目 tree | `ac13d559aae8f74e92afbf585fe79c5da02aa324` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.7` |
| 比较基线 | `v1.9.6` / `cdb5d7bec9c955227a1259281686e30377332fb3` |

完整来源、筛选范围和证据分层见 [来源与验证边界](SOURCE_PROVENANCE.md)。

来源 Tag 中新增的四章通用开发者指南、`22` 张信息图及 `2` 张界面参考图已经整理到仓库顶层
[TiRTC 开发者指南](../../../docs/README_CN.md#tirtc-开发者指南)。其余 `3` 份写作规范和专项架构材料
仍由开发侧维护，没有把内部过程记录混入应用使用说明。

## 文档

- [文档入口](docs/README_CN.md)
- [从 Wi-Fi 到体验功能](docs/GETTING_STARTED_CN.md)
- [源码构建与配置](docs/BUILD_AND_CONFIG_CN.md)
- [烧录与 OTA](docs/FLASH_AND_OTA_CN.md)
- [ThingConnect 接入与实现对齐](docs/THING_CONNECT_ALIGNMENT_CN.md)
- [设备呼叫流程](docs/DEVICE_CALL_FLOW_CN.md)
- [小钛接入说明](docs/ai_chat/README_CN.md)
- [音频和媒体所有权](docs/architecture/audio_media_ownership.md)
- [变更记录](CHANGELOG.md)

## 证据边界

来源快照、SDK 文件、公开源码、正式构建与 Release 资产已经用 commit、tree、文件清单和
SHA-256 绑定。目前没有 `1.9.7` 的烧录、串口启动、Wi-Fi、云端在线、
设备互呼、铃声听感或长时间稳定性证据；这些结果必须在目标板上分别观察，不能由静态检查代替。
