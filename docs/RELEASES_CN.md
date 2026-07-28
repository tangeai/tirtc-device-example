# 固件下载与校验

## 为什么固件不放在 Git

`.bin` 和 `.zip` 是构建产物。每次版本发布都会生成新的完整镜像，Git 无法像处理源码
那样有效保存它们的差异。长期提交会让克隆、拉取和 CI 明显变慢，即使后来从当前目录
删除，旧二进制仍会留在历史中。

本仓采用以下边界：

| 内容 | 保存位置 | 用途 |
| --- | --- | --- |
| 源码、文档、脚本、SDK 静态库 | Git `main` | 开发、审查和可复现构建 |
| 完整镜像、OTA app、维护包 | GitHub Releases | 长期下载、归档和人工烧录 |
| OTA manifest 和线上 app | OTA 服务 | 设备在线升级 |
| 临时 CI 构建结果 | GitHub Actions Artifact | 短期验证，不作为正式入口 |

GitHub Packages 更适合带包格式和依赖语义的软件包或容器。当前固件面向用户按版本直接
下载，GitHub Releases 的标签、说明、附件和校验文件更清晰。

## Release 必备附件

每个平台的正式 Release 至少包含：

| 附件 | 面向对象 | 说明 |
| --- | --- | --- |
| `*-full-vX.Y.Z.bin` | 普通体验者 | 16 MiB 完整镜像，ESP Launchpad 地址 `0x0` |
| `*-ota-vX.Y.Z.bin` | OTA 维护者 | 只包含 app 分区，不能替代完整首次烧录 |
| `*-webinstall-vX.Y.Z.zip` | 普通体验者 | 完整镜像和简明烧录说明 |
| `*-webflash-vX.Y.Z.zip` | 维护者 | bootloader、分区表、app、storage 和 offset |
| `SHA256SUMS.txt` | 所有人 | 下载完整性校验 |
| `release-manifest.json` | 校验工具 | 版本、commit、offset、大小和 SHA-256 |

本次统一源码 Release 还提供五个已完成干净构建的验证固件。它们按 `minimal-app`、
`device-monitor-app` 或 `rtos-with-spl` 命名，供开发者在对应目标板上继续集成验证；
首次完整烧录仍应选择与开发板匹配的 `*-full-*.bin`。

## 标签约定

平台和示例名称必须进入标签，避免 S3、P4 同版本互相覆盖：

```text
esp32-s3-trtc-advanced-v0.7.5
esp32-p4-trtc-advanced-v1.1.0
```

## 下载后校验

1. 从 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 选择与开发板
   和项目版本完全匹配的 Release。
2. 下载固件和同一 Release 中的 `SHA256SUMS.txt`。
3. 计算下载文件 SHA-256，并与清单逐字比对。
4. 首次烧录使用 `*-full-*.bin` 和地址 `0x0`；OTA 文件不能替代完整镜像。
5. 启动后核对串口或界面显示的应用版本、TiRTC SDK 版本和目标板型。

PowerShell：

```powershell
Get-FileHash .\downloaded-firmware.bin -Algorithm SHA256
```

Linux/macOS：

```bash
sha256sum ./downloaded-firmware.bin
```

Release 中的 manifest 用于核对 Tag、commit、项目路径、版本、源码文件清单和附件哈希。

静态校验可以证明发布记录之间一致，但不能单独证明 `.bin` 由对应源码构建。二进制来源需要
可复现构建、受控 CI，或固件内嵌 commit/版本元数据并进行比对。
