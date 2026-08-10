# ESP32-P4 完整设备应用：开发者上手指南

这份指南从一块能上电的 Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 开始，带你完成源码获取、
P4+C6 前置检查、构建、烧录、首次联网和基础功能确认。项目入口见 [README.md](../README.md)，
媒体链路细节见 [P4_MEDIA_ARCHITECTURE.md](P4_MEDIA_ARCHITECTURE.md)。

## 1. 先确认你拿到的是什么

| 项目 | 本版本要求 |
| --- | --- |
| 应用 | TiRTC ESP32-P4 完整设备应用 `1.3.1` |
| 目标板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| 主芯片 | ESP32-P4 |
| 网络芯片 | ESP32-C6，运行 ESP-Hosted slave |
| P4 与 C6 链路 | 4-bit、40 MHz SDIO |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 定制兼容快照 |
| Flash | `16MB` |
| 网络 | 2.4 GHz Wi-Fi，可访问 TiRTC/ThingConnect 服务 |
| 交付形式 | 源码与 `0x0` 完整烧录镜像 |

至少准备：目标板、可靠的 USB 数据线、稳定供电、能进入下载模式的 P4 USB/串口口，以及
一台使用 Chrome 或 Edge 的电脑。直接烧录完整镜像不要求安装 ESP-IDF；修改源码时再安装
Git 和 ESP-IDF `5.5.4`。摄像头、LCD、触摸和音频 codec 应按目标板原理图连接。

## 2. 理解 P4 和 C6 的边界

P4 负责 UI、摄像头、音频、编解码和 TiRTC 应用；C6 只负责 Wi-Fi。当前 P4 配置使用：

- ESP-Hosted `1.4.7`。
- ESP Wi-Fi Remote `0.14.x` 协议族。
- SDIO CLK `18`、CMD `19`、D0-D3 `14/15/16/17`、C6 reset `54`。

这些是 Waveshare 目标板的固定板级配置。P4 工程构建或烧录不会自动更新 C6。

已有板卡能在原固件中扫描 Wi-Fi 时，先保留 C6 不动。若新 P4 固件启动后始终没有 AP 列表，
再检查 C6 slave 固件、C6 下载口、SDIO 连线和 reset 极性。ESP-Hosted 的上游说明保留在
[components/espressif__esp_hosted/README.md](../components/espressif__esp_hosted/README.md)。
不要把 P4 APP BIN 烧进 C6，也不要为了排查 Wi-Fi 先改摄像头或 TiRTC 代码。

## 3. 获取固定源码

推荐从统一公开仓的发布 Tag 开始，而不是直接跟随不断变化的 `main`：

```powershell
git clone https://github.com/tangeai/tirtc-device-example.git
cd tirtc-device-example
git checkout esp32-p4-device-monitor-v1.3.1
cd complete-applications/esp32-p4/device-monitor
```

核对项目身份：

```powershell
git describe --tags --always
Select-String -Path CMakeLists.txt -Pattern 'PROJECT_VER'
Select-String -Path components/tirtc_sdk/include/tiRTC.h -Pattern 'TIRTC_VERSION_'
```

预期应用版本为 `1.3.1`，TiRTC SDK API 版本为 `2.3.0`。本项目使用包含 HTTP DNS disable
回移的 ESP32-P4 定制兼容快照，不能只看版本号替换静态库。完整 commit 和哈希见
[VERSION.md](../VERSION.md)。

## 4. 安装并进入 ESP-IDF 5.5.4

请使用 Espressif 官方 ESP-IDF `5.5.4`。同一台电脑安装过多个 IDF 时，先确认当前 shell
没有继承另一个版本的工具链。

PowerShell：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py --version
```

Linux 或 WSL：

```bash
. "$IDF_PATH/export.sh"
idf.py --version
```

`idf.py --version` 应显示 `v5.5.4`。首次配置时，ESP-IDF Component Manager 还会下载
未随源码仓提交的托管组件，因此需要能访问 Espressif 组件仓库。下载失败时先处理网络、代理
或证书问题，不要随意换成其他版本的 LVGL、ESP-SR、Wi-Fi Remote 或视频组件。

## 5. 建立干净构建目录

在项目根目录执行：

```powershell
idf.py set-target esp32p4
idf.py reconfigure
idf.py build
```

Linux/WSL 使用相同的 `idf.py` 命令。首次构建时间较长属于正常现象，项目包含 LVGL、
ESP-Hosted、ESP-SR、H264/JPEG、摄像头和较大的图片/文字资源。

构建完成后应看到 `Project build complete`，并生成：

```text
build/flasher_args.json
build/bootloader/bootloader.bin
build/partition_table/partition-table.bin
build/tirtc_esp32p4_device_app.bin
```

工程还会生成 OTA data 和 `storage` SPIFFS 镜像。实际烧录文件、offset、flash mode 和 flash
size 以本次构建的 `build/flasher_args.json` 为准，不要从本文、旧日志或其他工程手抄地址。

如果修改了 IDF 版本、目标芯片、分区或大块 Kconfig 配置，先清理再构建：

```powershell
idf.py fullclean
idf.py set-target esp32p4
idf.py build
```

## 6. 配置应该放在哪里

### 6.1 普通使用：在设备屏幕上配置

推荐路径是不把凭据编译进固件：

1. 在“设置 -> Wi-Fi 设置”扫描并连接网络。
2. Wi-Fi SSID 和密码保存在 NVS，重启后自动重连。
3. 设备联网后自动申请 6 位绑定码；按屏幕二维码或网址在平台完成绑定。
4. 绑定下发的设备身份保存在 NVS；“设置 -> TiRTC 配置”可以查看状态或重置绑定。
5. 微信联系人、设备联系人和 AI 角色由对应平台账号与授权关系提供。

### 6.2 源码配置：只改产品策略

运行开关位于：

- `main/Kconfig.projbuild`：AEC、AI 视频、微信音视频、H264 档位、弱网策略和诊断日志。
- `sdkconfig.defaults`：板卡、PSRAM、SDIO、分区和稳定默认值。
- `main/application/app_config.h`：服务入口和空的编译期凭据占位。

需要交互调整时执行：

```powershell
idf.py menuconfig
```

稳定配置已经启用 AEC、AI 视频、微信本地/远端视频和 direct-LCD；SDK 码率自适应、旧本地
自动弱网降级、逐帧日志和 LAN 调试服务器默认关闭。没有明确验证目标时，先保留默认值。

不要提交真实 Wi-Fi 密码、device secret、access key、token 或平台账号。即使只是临时联调，
也优先使用屏幕配置和 NVS。

## 7. 使用 Espressif Web Serial 烧录 P4

### 7.1 快速体验：烧录 Release 完整镜像

从 [`esp32-p4-device-monitor-v1.3.1` Release](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-device-monitor-v1.3.1) 下载：

```text
esp32p4-tirtc-device-monitor-full-v1.3.1.bin
```

文件大小应为 `16,777,216` bytes，SHA-256 应为：

```text
3F55403C60CE371D81239CD5EC028FBFD1A27CAC0B693EB925CB1C60F3CFE1C5
```

PowerShell 可执行：

```powershell
Get-FileHash -Algorithm SHA256 .\esp32p4-tirtc-device-monitor-full-v1.3.1.bin
```

然后：

1. 使用 Chrome 或 Edge 打开 [Espressif Web Serial 烧录工具](https://espressif.github.io/esptool-js/)。
2. 连接 P4 下载口；自动进入下载模式失败时，按住 `BOOT`，点按 `RESET`，再松开 `BOOT`。
3. 选择浏览器弹出的 P4 串口，不要误选 C6 端口。
4. 添加完整镜像，烧录地址填 `0x0`。
5. Flash Size 选择 `16MB`，Flash Mode/频率使用 `DIO/80MHz`。
6. 开始烧录，校验完成后复位 P4。

完整镜像包含 bootloader `0x2000`、partition table `0x8000`、空 otadata `0xd000`、APP
`0x10000` 和 storage `0xe80000`。NVS 区域为 `FF`，所以完整烧录一定会清除原有 NVS、Wi-Fi
和设备绑定。重启后从第 8 节重新配网和绑定，这是预期行为。

### 7.2 源码开发：烧录本地构建分片

需要修改代码时，先完成第 5 节的本地构建，再打开 `build/flasher_args.json`，逐项添加其中
列出的 BIN 和 offset。Flash mode、频率和大小均以本次构建记录为准。

不要把 `build/tirtc_esp32p4_device_app.bin` 单独写到 `0x0`。它只是应用镜像，还需要
bootloader、partition table、OTA data 和 `storage` 镜像。只有上面的 Release `full` 镜像
是可以直接从 `0x0` 烧录的完整文件。

需要串口日志时执行：

```powershell
idf.py -p <PORT> monitor
```

Windows 端口示例为 `COM7`，Linux 示例为 `/dev/ttyACM0`；请使用电脑实际枚举的 P4 端口，
不要误选 C6。按 `Ctrl+]` 退出 monitor。

## 8. 第一次启动

### 8.1 串口身份

复位后先看固件身份：

```text
firmware version: 1.3.1 project=tirtc_esp32p4_device_app ...
system ready: ESP32-P4 TiRTC dashboard
```

如果版本或项目名不一致，先停下来确认端口、构建目录和烧录文件；不要继续用错误固件排查业务。

### 8.2 屏幕和触摸

应出现横屏主界面或首次绑定提示，触摸方向应与画面一致。黑屏、花屏或触摸坐标旋转通常是
板型、排线、供电或 BSP 配置问题，和 TiRTC 凭据无关。

### 8.3 连接 Wi-Fi

首次启动没有保存的网络时：

1. 点击绑定提示中的“设置WiFi”，或进入“设置 -> Wi-Fi 设置”。
2. 等待 AP 列表出现，选择 2.4 GHz 网络。
3. 输入密码并连接。
4. 串口应出现 `wifi connected: ssid=... ip=...`，屏幕应显示“已连接 Wi-Fi”。

AP 列表能出现，才说明 P4、C6、ESP-Hosted 和 SDIO 已经走通到 Wi-Fi 扫描阶段。

### 8.4 完成绑定

联网后设备会请求临时会话。MQTT 临时订阅完成后，屏幕显示 6 位绑定码和二维码，串口可见：

```text
binding verification code ready: mqtt subscribed
```

按屏幕显示的网址或二维码完成平台确认。绑定成功后设备保存身份并启动正式在线服务。
如果刚重置过绑定，旧身份和旧联系人关系可能需要在平台侧重新确认。

### 8.5 先跑两个内置检查

进入“设置”：

1. “网络测试”确认基础网络可达。
2. “TiRTC 测试”依次观察获取 Token、连接房间、发送音频、接收音频和断开测试。

这两个页面能快速区分“Wi-Fi 还没通”和“业务账号/RTC 链路还没通”，比直接进入视频场景
更容易定位问题。

## 9. 按场景验证核心能力

| 场景 | 操作主线 | 最少应确认的现象 |
| --- | --- | --- |
| IPC 查看 | 首页“查看”，由 Web/平台订阅设备 | P4 H264 首帧上行；远端画面持续更新；设备不显示本地摄像头预览 |
| 设备呼叫 | 首页“设备呼叫”，选择联系人和音频/视频类型 | 呼叫状态完整切换；音频双向；视频呼叫上、下行分别有首帧 |
| 微信 VoIP | 添加已授权微信联系人，发起或接听音频/视频呼叫 | 音频呼叫不启摄像头；视频呼叫可见本地 H264 上行和 P4 MJPEG 下行 |
| AI Chat | 首页“AI 对讲”，开始新对话 | 本地音频发送、AI 音频播放、字幕更新；侧键可打断回复 |
| AI 设备动作 | 在已授权角色中请求查询或呼叫联系人 | 返回明确的接受/失败原因；应用切换到对应设备或微信呼叫页 |
| OTA | “设置 -> 关于 / OTA” | 能读取当前版本和检查状态；真正升级前另行确认 OTA 服务与签名策略 |

验证视频时按当前工程参数判断，不要混用方向：

- IPC 的 P4 上行是 `1280x960@20fps`、`4Mbps` H264，GOP 为 `40` 帧 / `2s`。
- 设备呼叫和微信 VoIP 的 P4 上行是 `480x320@15fps`、`800kbps` 起始 H264，
  GOP 为 `30` 帧 / `2s`。
- 微信服务端到 P4 的下行请求是 `640x480` MJPEG，P4 硬解后居中 `cover` 到 `480x320`。
- 本工程没有配置或证明微信手机端为 720p；手机采集档位由微信和服务端决定。

一次看到首帧还不够。继续观察帧率、码率、音频连续性、队列压力，再连续进入/退出场景，
确认摄像头、音频、显示和连接都能被下一次会话重新获取。

## 10. 用 1.3.1 的日志定位第一处异常

1.3.1 把关键日志按媒体阶段拆开。出现黑屏或卡顿时，先找最早停止推进的一段，不要直接
重启、降分辨率或关闭功能。

### 10.1 摄像头和上行节拍

首帧日志包含 `seq` 和 `drain`；周期统计包含 `seq_delta`、`avg_gap_us`、capture、encode、
callback 耗时以及 internal/DMA/PSRAM 水位。

- `seq` 持续递增：V4L2 正在交付新帧。
- `drain` 增加：driver 排出了已经完成但过旧的帧，避免把积压帧当实时画面发送。
- `seq_delta` 大于 1：传感器或 driver 在两次应用取帧之间完成了多帧；要结合目标 fps 和
  `drain` 判断，不能只凭它认定丢帧。
- `avg_gap_us` 明显偏离目标：继续对照 capture、encode 和 callback 耗时，找到节拍先被谁拖慢。

应用节拍使用向上取整的帧间隔并保持原相位：15fps 约为 `67ms`，20fps 为 `50ms`。单次超时
只跳过已经错过的周期，不会从当前时刻重新起算并不断漂移。

### 10.2 TiRTC 上行和远端下行

`video tx liveness` 会给出 `enq/deq/api/ok` 四个年龄：

- `capture`：摄像头是否仍在产生帧。
- `queue`：帧是否进入并离开发送队列。
- `api`：是否已经调用 SDK 发送接口。
- `sdk`：SDK 是否仍返回成功，以及发送 buffer 水位。

`VRX stall stage=transport` 表示订阅后远端包没有继续到达；
`VRX stall stage=renderer` 表示 SDK callback 已经收到包，但 renderer 提交没有继续推进。
前者优先查订阅、对端和网络，后者再查 codec、输入池和 renderer。

### 10.3 H264 下行 decoder

`video stall stage=input` 表示输入帧间隔先拉长；`stage=decode` 表示 TinyH264 处理单个 access
unit 先变慢。周期统计会分别显示 receive、queued、decoded、converted、presented fps，
以及 input/display 队列、阶段耗时和 PTS 回退计数。

TinyH264 helper 运行在 CPU1，优先级高于同步等待它的 caller。同步保护会等待 helper 消费
前一阶段，再送达原阶段值；看到 `TinyH264 sync guard` 说明保护路径发生过等待，不等于业务
已经失效。

若出现 `H264 decoder stalled ... quarantined`，说明一次 decode 已阻塞至少 2 秒。系统会停止
继续向该 decoder 投喂，并记录 caller、helper、音频 capture/playback 的任务状态。只有原调用
最终返回后，才能安全销毁 decoder 并从新 IDR 重建；永久不返回时的回收仍需长稳验证，不能
仅凭后续 UI 可操作就认定 decoder 已恢复。

### 10.4 内存水位

`memory waterline` 分为 `normal`、`warning`、`critical`。它同时检查 internal free、internal
largest block、PSRAM free 和 PSRAM largest block，并记录历史 minimum 与 PSRAM 分配失败次数。
健康状态不会周期刷屏，只在水位变化、恢复或新分配失败时记录。

一次进入通话时下降不等于泄漏。连续退出并重新进入场景，观察水位能否回到稳定区间；如果
free 恢复但 largest block 持续下降，优先检查碎片化和大块资源生命周期。

### 10.5 微信 worker

微信拒绝、断开等 SDK 耗时操作进入固定 work worker 串行执行；接听 worker 使用 PSRAM 大栈
常驻，并用请求序号隔离已取消或已被新请求替代的任务。遇到接听问题时依次看请求序号、
worker 是否唤醒、会话 generation 是否仍有效，再看 SDK 返回值。

## 11. 常见问题

| 现象 | 先检查 | 下一步 |
| --- | --- | --- |
| `idf.py` 不是 5.5.4 | 当前 shell 的 `IDF_PATH` | 重新执行 5.5.4 的 `export.ps1`/`export.sh` |
| 配置阶段下载组件失败 | 网络、代理、证书、组件仓可达性 | 恢复下载，不要随意替换依赖版本 |
| 构建找不到 TiRTC 符号 | `components/tirtc_sdk/` 是否完整 | 用 [VERSION.md](../VERSION.md) 的哈希核对 SDK，不要混入 S3 库 |
| 浏览器连不上芯片 | 端口、数据线、下载模式 | 确认选择 P4 端口，手动 `BOOT` + `RESET` 进入 ROM 下载模式 |
| 烧录后无法启动 | `flasher_args.json`、offset、flash size | 重新烧录全部分片；不要把应用 BIN 写到 `0x0` |
| 屏幕亮但 Wi-Fi 列表为空 | C6 slave、SDIO、reset、C6 固件兼容性 | 先恢复 ESP-Hosted；TiRTC 和账号配置此时还没参与 |
| Wi-Fi 已连但不显示绑定码 | 时间、DNS、服务发现、临时 MQTT | 看 binding/HTTP/MQTT 的首个错误，不要反复擦 Flash |
| 绑定成功但 TiRTC 测试失败 | 设备身份、Token、服务可达性 | 先用内置 TiRTC 测试定位 token、房间或媒体阶段 |
| IPC 远端黑屏 | 摄像头启动、H264 首帧、订阅状态 | 按 camera -> encoder -> TiRTC -> subscriber 分段看首帧 |
| 微信视频黑屏 | 先分清上行还是下行 | 上行查 `480x320` H264；下行查 `640x480` MJPEG 请求、首包和 JPEG 解码 |
| 微信画面比例不对 | 服务端实际下发尺寸和方向 | 记录实际 MJPEG 尺寸；不要按“720p”假设修改 P4 缩放 |
| 通话有明显回声 | AEC 是否 active、reference 是否有效 | 有同步录音证据后再调整 `APP_AUDIO_AEC_REF_DELAY_MS` |
| 视频周期卡一下 | GOP、`avg_gap_us`、`video tx liveness` | 先确认 IPC 是 40 帧、通话是 30 帧 GOP，再定位采集、队列、SDK 哪一段先停 |
| 下行停住但 UI 还能操作 | `VRX stall` 与 `video stall` 的 stage | 区分 transport、renderer、input、decode，不要用 UI 存活代替 decoder 存活 |
| 出现 TinyH264 sync guard | 等待时间和后续 decoded/presented fps | 这是同步保护证据；继续确认链路是否恢复，不要把该日志本身当成错误或成功 |
| 切换场景后内存下降 | 队列、PSRAM slot、连接 owner 是否归零 | 连续进入/退出并记录 internal/PSRAM largest block 与 task HWM |

## 12. 开发时从哪里改

| 目标 | 主要目录 |
| --- | --- |
| 业务进入/退出、跨应用切换 | `main/application/` |
| Wi-Fi 和网络状态 | `main/connectivity/` |
| 摄像头、音频、显示、触摸 | `main/drivers/`、`main/hardware/` |
| 编码、像素转换、媒体资源 | `main/media/` |
| TiRTC 连接、订阅和媒体队列 | `main/protocols/tirtc/` |
| 绑定、在线、设备呼叫、微信、AI、OTA | `main/services/` |
| 页面、交互和图片文字资源 | `main/ui/` |

先找到资源的唯一所有者，再改逻辑。UI 不应释放 RTC 连接，SDK 回调不应直接调整硬件，
显示丢帧也不应关闭会话。完整所有权规则见
[P4_MEDIA_ARCHITECTURE.md](P4_MEDIA_ARCHITECTURE.md)。

媒体数值优先改 `main/media/media_tuning.h` 和
`main/services/call_video_renderer_config.h`。Kconfig 用于构建组成与硬件开关，生成的
`sdkconfig` 不应成为运行时媒体策略的唯一事实源。

## 13. 这份发布已经证明到哪里

公开源码固定到来源 Tag 和 commit，应用/SDK 版本及静态库哈希已有记录。公开候选已在
ESP-IDF `5.5.4` 完成一次干净构建：应用镜像 `6,924,512` bytes，SHA-256
`EBD5FE3B930BA000FDBE7094F287AD66CBB745D56F8D167ED4890895A691DFA5`，编译错误为 0，最小
app 分区剩余 `0x95720` bytes（8%）。正式完整镜像只上传 GitHub Release，不进入 Git；大小
`16,777,216` bytes，SHA-256
`3F55403C60CE371D81239CD5EC028FBFD1A27CAC0B693EB925CB1C60F3CFE1C5`。

干净构建证明候选能在记录的环境中完成编译和链接。烧录成功、C6/SDIO 可用、平台在线、
音视频首帧和长稳结果仍要按上面的步骤在目标板上分别确认。这样记录问题时，大家能立刻知道
证据停在哪一层，不必从头猜。TinyH264 同步保护、decoder 隔离和返回后的重建都需要重点做
连续呼叫、故障注入和长稳验证。
