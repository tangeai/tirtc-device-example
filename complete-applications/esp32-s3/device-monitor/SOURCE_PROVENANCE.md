# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `esp32s3_tirtc_device_monitor_demo` |
| 源分支 | `main` |
| 源标签 | `v1.7.6` |
| 源提交 | `04dfca5345f3b3ae6b0afa94e8a4d1d49986d214` |
| 应用版本 | `1.7.6` |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.2.0` |
| TiRTC SDK BuildInfo commit | `1df9e045a9dc` |
| TiRTC 静态库 SHA-256 | `0686e5d7f5bfea18b7b3ee6ae1701061b3afcfb0ef2030642ebebab88af0413d` |

本目录按 `v1.7.6` 指向的不可移动源码快照整理。服务发现入口在源码和上手文档中统一为
`http://ep-open.tangeopen.com/services`。

## 公开筛选

统一仓保留应用源码、必要组件、可复用配置、公开开发者文档和构建辅助脚本。以下内容不进入
公开源码快照：

- 来源仓 Git 元数据、本地治理 Skill、IDE 配置和完整 `sdkconfig`。
- `build/`、`build-current/`、`managed_components/`、日志、NVS 转储和固件镜像。
- 内部设计交接、调试截图、生成预览、旧版本发布记录和其他一次性维护资料。
- 真实 Wi-Fi、设备身份、密钥、Token 和个人工作区路径。

## 验证边界

来源 Tag 固定应用版本、业务源码和 TiRTC SDK 快照。统一公开 Release 的构建产物、文件大小、
SHA-256、烧录地址和统一仓 commit 由当次 `release-manifest.json` 记录；目标板运行结果使用
独立的烧录、串口、联网和业务验证证据。
