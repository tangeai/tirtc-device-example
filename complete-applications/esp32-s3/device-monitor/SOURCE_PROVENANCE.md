# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `esp32s3_tirtc_device_monitor_demo` |
| 源分支 | 隔离发布候选 |
| 源标签 | `v1.8.1` |
| 源提交 | `9aff1adedd5139b2c46b96ba2a743783ead9f1bf` |
| 比较基线 | `v1.8.0` / `8211e3bec7a40beff5cfe81dbf9cb8468016e786` |
| 应用版本 | `1.8.1` |
| 发布日期 | `2026-08-18` |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.2.0` |
| TiRTC SDK BuildInfo commit | `1df9e045a9dc` |
| TiRTC 静态库 SHA-256 | `0686e5d7f5bfea18b7b3ee6ae1701061b3afcfb0ef2030642ebebab88af0413d` |

本目录按 `v1.8.1` 指向的不可移动源码快照整理。服务发现入口在源码和上手文档中统一为
`http://ep-open.tangeopen.com/services`。

## 补丁范围

- ESP-IDF 工程版本由 `1.8.0` 更新为 `1.8.1`。
- 设备主动呼叫微信联系人时，`wx_version_type` 默认值由开发版 `1` 改为体验版 `2`。
- 新增正式版、开发版、体验版具名常量和编译期取值检查。
- 其余业务代码、板级配置、分区表和 TiRTC SDK 与 `v1.8.0` 保持一致。

## 公开筛选

统一仓保留应用源码、必要组件、可复用配置、公开开发者文档和构建辅助脚本。以下内容不进入
公开源码快照：

- 来源仓 Git 元数据、本地治理 Skill、IDE 配置和完整 `sdkconfig`。
- `build/`、`build-current/`、`managed_components/`、日志、NVS 转储和固件镜像。
- 内部设计交接、调试截图、生成预览、旧版本发布记录和其他一次性维护资料。
- 真实 Wi-Fi、设备身份、密钥、Token 和个人工作区路径。

## 验证边界

统一候选已在 ESP-IDF `5.5.4`、Xtensa 工具链 `14.2.0_20260121` 下执行一次正式干净
构建：

```powershell
idf.py -B <全新构建目录> --no-ccache -D SDKCONFIG=<独立 sdkconfig> reconfigure build
```

构建完成的 app 为 `7,449,184` bytes，SHA-256 为
`b552e3971a0fab7999384f6af0c841b779913c11227cde4c03f6cc3a143f6e3c`，能够放入
`0x770000` app 分区，剩余 `349,600` bytes。构建日志中编译错误和编译器 warning 均为
`0`；CMake 报告 `14` 条 Windows 路径长度提示，因此从源码构建时建议使用较短的工作目录，
并持续关注图片、字体等资源带来的分区余量。

来源 Tag 固定应用版本、业务源码和 TiRTC SDK 快照。统一公开 Release 的构建产物、文件大小、
SHA-256、烧录地址和统一仓 commit 在正式构建后写入当次 `release-manifest.json`；目标板运行
结果使用独立的烧录、串口、联网和业务验证证据。上述构建记录说明候选代码能够完成编译和
链接；开发者仍应按 README 的首次体验顺序观察屏幕、联网、绑定和业务功能。
