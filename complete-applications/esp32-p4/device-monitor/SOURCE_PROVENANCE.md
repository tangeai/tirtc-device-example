# 源码来源与发布边界

本文记录 TiRTC ESP32-P4 完整设备应用在统一示例仓中的来源快照、公开筛选方式和证据边界。
开发者可以据此确认“代码从哪里来”，也能知道哪些事实仍要在自己的构建和目标板上验证。

## 来源快照

| 项目 | 内容 |
| --- | --- |
| 应用版本 | `1.3.1` |
| 来源 Tag | `esp32-p4-device-app-v1.3.1` |
| Tag object | `9e08fffb1d6a4b200cf97af299c9e20a1fba1f24` |
| 来源 commit | `739146438dd4b65512bb8198731bd2c8a1eb1275` |
| 比较基线 | `esp32-p4-device-app-v1.3.0` / `dacf8e65b25ea4d0282fc9314e46a18607691fb4` |
| 公开项目 Tag | `esp32-p4-device-monitor-v1.3.1` |
| 公开路径 | `complete-applications/esp32-p4/device-monitor/` |
| 目标开发板 | Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.3.0` ESP32-P4 定制兼容快照 |

来源 Tag 是本次公开整理的唯一源码基线。来源仓 Tag 之外的工作树状态、`build/`、本地日志
和未提交内容都不属于公开范围。

来源 `1.3.0..1.3.1` 共变更 `41` 个文件，主要包括：

- 摄像头 sequence 去重、旧完成帧排出、目标 fps 锁相节拍和 2 秒 GOP。
- TinyH264 双任务同步通知保护、解码阶段统计与阻塞诊断。
- 本地发送和远端接收 liveness、阶段化 stall 记录、内存水位和资源快照。
- 微信固定工作 worker、常驻接听 worker 和请求序号隔离。
- 媒体参数集中管理，以及 TiRTC HTTP DNS disable 兼容回移。

## 公开筛选

来源 Tag 包含 `1,201` 个文件。统一仓保留其中 `1,199` 个来源路径，并额外维护两份面向
外部开发者的文档：本文件和 `docs/GETTING_STARTED_CN.md`。

未纳入的两个来源文件：

- `.clangd`：来源开发机的编辑器索引配置。
- `sdkconfig`：来源构建目录生成的完整配置；可复用默认值由 `sdkconfig.defaults` 提供。

统一仓保留应用源码、第三方组件、TiRTC SDK、必要配置、辅助脚本和开发者文档。以下内容
不会进入公开源码快照：

- 来源仓 Git 元数据、内部协作文档和本机 IDE 配置。
- `build/`、`managed_components/`、日志、固件镜像和发布附件目录。
- P4 APP BIN、ELF、MAP 或其他本地构建输出不进入 Git；正式完整镜像单独作为 Release 资产。

README、VERSION、开发者上手指南和媒体架构文档按统一仓风格整理。其余保留来源路径用于
逐文件一致性校验；Windows 与 Git 之间可能出现 CRLF/LF 工作树表现差异，发布校验以 Git
blob 字节或统一换行后的文本内容为准，不能仅看工作树时间戳。

## TiRTC SDK 字节契约

本版本使用的 SDK 不是仅凭 `2.3.0` 字样即可替换的通用包，而是 ESP32-P4 定制兼容快照：

| 项目 | 内容 |
| --- | --- |
| Nano baseline | `aaad3da251bac90e0642b51b3279a1f40ca9fa9a` |
| HTTP DNS disable backport | `fde4f1c58d2dc28f3d3d04e25dd49bc3a399fea6` |
| `libTiRTC.a` SHA-256 | `b0a38061b0c63ad0c556f73bb2ecc47c6fd84823b7524f774fe09916f577b4c6` |
| `libTiRTC.a` MD5 | `13c36e22805776d0f437f16dddda87ad` |
| `tiRTC.h` SHA-256 | `b8338a07532e09a8f5ccd6f0270a9d1fa3f227f8bc1a57fd663115b222984801` |

HTTP DNS disable 回移保留公开 `2.3.0` API 和 TGMP 回调契约，但让 `/v1/connect` 使用平台
DNS resolver，以避开 SDK 自定义 DNS 缓存过期时的递归锁。

为了保持供应方静态库字节和上述 SHA-256，统一仓没有 strip 或重写 `libTiRTC.a`。该归档的
调试信息仍可能包含供应方的 Linux/WSL 源码目录、ESP-IDF 路径和工具链路径。这些字符串
不是设备运行时路径，也不是 Wi-Fi 或 TiRTC 凭据；但它们确实是发布快照的一部分，因此本
文明确披露，不能笼统宣称整个仓库“没有任何绝对路径”。

首方源码、配置和公开文档仍应通过凭据与个人路径扫描；SDK 二进制按已披露例外和固定哈希
验收。若供应方以后提供去除调试路径的新归档，应作为新的 SDK 快照重新记录版本与哈希，
不能在消费仓中就地修改当前静态库。

## 发布和验证边界

本项目同时发布源码和 `esp32p4-tirtc-device-monitor-full-v1.3.1.bin`。完整镜像只上传 GitHub
Release，不进入 Git；大小为 `16,777,216` bytes，SHA-256 为
`3F55403C60CE371D81239CD5EC028FBFD1A27CAC0B693EB925CB1C60F3CFE1C5`，烧录地址为 `0x0`。
当前文档与静态校验可以证明：

- 公开路径来自明确的来源 Tag 和 commit。
- SDK 版本、兼容回移和关键文件哈希可重复核对。
- 构建产物、开发机配置和真实凭据不属于公开源码范围。
- ESP-IDF `5.5.4` 干净构建完成，应用镜像为 `6,924,512` bytes，SHA-256 为
  `EBD5FE3B930BA000FDBE7094F287AD66CBB745D56F8D167ED4890895A691DFA5`，编译错误为 0。
- 完整镜像中的 bootloader、partition table、空 otadata、APP 和 storage 分别对应构建输出
  `0x2000`、`0x8000`、`0xd000`、`0x10000` 和 `0xe80000`；NVS 区为 `FF`。

这些证据不能代替：

- P4 烧录、C6 ESP-Hosted/SDIO、Wi-Fi、绑定和平台在线。
- IPC、设备呼叫、微信 VoIP、AI Chat、AEC 和长期运行。
- TinyH264 永久阻塞时的安全回收和连续多轮呼叫稳定性。

开发者应按[开发者上手指南](docs/GETTING_STARTED_CN.md)分别记录烧录、联网、首帧和长稳证据。
这样可以把“源码与构建一致”同“目标板已验证”清楚分开。
