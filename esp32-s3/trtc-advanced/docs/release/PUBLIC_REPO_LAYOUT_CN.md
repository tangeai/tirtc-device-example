# 统一设备示例仓

公开仓库：

<https://github.com/tangeai/tirtc-device-example>

## 目录

```text
tirtc-device-example/
├── esp32-s3/
│   ├── trtc-basic-call/
│   └── trtc-advanced/
├── esp32-c3/
├── esp32-p4/
├── s10/
├── realtek/
├── common/
└── docs/
```

当前工程发布到：

```text
esp32-s3/trtc-advanced/
```

S3 与 P4 是同级平台，不能互相嵌套。每个平台保留自己的板级驱动、构建配置和版本说明；
只有确认被两个及以上平台复用的协议、工具和文档才进入 `common/`。

## Git 保存什么

Git 保存：

- 可构建源码
- README 和接入文档
- 构建、校验和发布脚本
- 必要的 TiRTC SDK 头文件和平台静态库
- UI 图片、字体和其他运行期必需资源
- Release 说明和版本元数据

Git 不保存：

- `build/` 和临时构建目录
- `.bin/.zip` 固件产物
- `release_assets/`、`artifacts/`、`releases/`
- 串口日志、抓包、截图临时文件和个人 IDE 配置
- Wi-Fi 密码、设备密钥、token、私钥和个人联系人标识

## 固件放在哪里

正式固件发布到：

<https://github.com/tangeai/tirtc-device-example/releases>

推荐平台作用域标签：

```text
esp32-s3-trtc-advanced-vX.Y.Z
esp32-p4-trtc-advanced-vX.Y.Z
```

每个 Release 至少包含：

- 普通体验者使用的 `0x0` 完整镜像
- 维护者多地址烧录 zip
- OTA app，平台支持 OTA 时提供
- `SHA256SUMS.txt`
- `release-manifest.json`
- 发布说明和验证边界

GitHub Actions artifact 只用于短期构建验证，不能作为长期正式下载地址。OTA 服务仍保存
在线升级所需 app 和 manifest；GitHub Release 负责公开下载、人工烧录和版本归档。

## 发布顺序

1. 在平台权威源工程完成开发。
2. 检查工作区范围和敏感信息。
3. 使用目标 ESP-IDF 环境重新构建。
4. 记录源码 commit、SDK 版本、app 大小和 SHA-256。
5. 同步源码到统一仓对应平台目录，不复制 `.git`、`build` 或固件包。
6. 在统一仓再次检查 diff 和文档链接。
7. 提交源码和文档。
8. 创建平台作用域标签。
9. 从已验证 build 生成 Release 资产并上传。
10. 清理临时资产目录，确认 Git 中没有 `.bin/.zip`。
11. 分别记录构建、Release 下载、网页烧录、OTA 和真机功能验证结果。
