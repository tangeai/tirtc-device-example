# 发布与二进制管理

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
| `release-manifest.json` | 发布工具 | 版本、commit、offset、大小和 SHA-256 |

## 标签约定

平台和示例名称必须进入标签，避免 S3、P4 同版本互相覆盖：

```text
esp32-s3-trtc-advanced-v0.7.5
esp32-p4-trtc-advanced-v1.1.0
```

## 发布检查

1. 确认源工程版本、SDK 版本和 Release 标签一致。
2. 在干净或明确记录差异的工作树中完成构建。
3. 根据 `flasher_args.json` 生成资产，不手写 offset。
4. 生成并复核 `release-manifest.json`、`SHA256SUMS.txt`。
5. 确认 `git ls-files "*.bin" "*.zip"` 无固件产物。
6. 复核 README 中的开发板、下载文件名、地址 `0x0` 和 H5 流程。
7. 推送源码与标签。
8. 创建 GitHub Release 并上传附件。
9. 从远端重新下载附件，复算 SHA-256。
10. 把构建通过和真机通过分开记录；未烧录、未运行的测试不得写成通过。
