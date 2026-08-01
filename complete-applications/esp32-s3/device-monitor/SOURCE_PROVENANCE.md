# 来源与验证边界

## 来源快照

| 项目 | 值 |
| --- | --- |
| 源工程 | `esp32s3_tirtc_device_monitor_demo` |
| 源分支 | `main` |
| 源标签 | `v1.8.0` |
| 源提交 | `8211e3bec7a40beff5cfe81dbf9cb8468016e786` |
| 比较基线 | `v1.7.6` / `04dfca5345f3b3ae6b0afa94e8a4d1d49986d214` |
| 应用版本 | `1.8.0` |
| 发布日期 | `2026-07-31` |
| ESP-IDF | `5.5.4` |
| TiRTC SDK | `2.2.0` |
| TiRTC SDK BuildInfo commit | `1df9e045a9dc` |
| TiRTC 静态库 SHA-256 | `0686e5d7f5bfea18b7b3ee6ae1701061b3afcfb0ef2030642ebebab88af0413d` |

本目录按 `v1.8.0` 指向的不可移动源码快照整理。服务发现入口在源码和上手文档中统一为
`http://ep-open.tangeopen.com/services`。

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
idf.py -B build-release-20260731 --no-ccache reconfigure build
```

构建完成的 app 为 `7,449,184` bytes，能够放入 `0x770000` app 分区，剩余 `349,600`
bytes。构建日志没有编译错误；Windows 环境出现路径长度提示，因此从源码构建时建议使用
较短的工作目录，并持续关注图片、字体等资源带来的分区余量。

来源 Tag 固定应用版本、业务源码和 TiRTC SDK 快照。统一公开 Release 的构建产物、文件大小、
SHA-256、烧录地址和统一仓 commit 在正式构建后写入当次 `release-manifest.json`；目标板运行
结果使用独立的烧录、串口、联网和业务验证证据。上述构建记录说明候选代码能够完成编译和
链接；开发者仍应按 README 的首次体验顺序观察屏幕、联网、绑定和业务功能。
