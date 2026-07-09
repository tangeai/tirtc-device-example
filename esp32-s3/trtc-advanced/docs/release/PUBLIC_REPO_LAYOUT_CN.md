# 公开仓库目录规划

未来公开远端仓库：

<https://github.com/tangeai/tirtc-device-example>

仓库名：

`tirtc-device-example`

## 目标目录结构

```text
tirtc-device-example/
├── esp32-s3/               # ESP32-S3 平台示例
│   ├── trtc-basic-call/    # 基础音视频通话
│   └── trtc-advanced/      # 高级功能示例
├── esp32-c3/               # ESP32-C3 平台示例
├── esp32-p4/               # ESP32-P4 平台示例
├── s10/                    # 君正 S10 平台示例
├── realtek/                # Realtek 平台示例
├── common/                 # 跨平台公共组件
└── docs/                   # 跨平台文档、接入说明和发布说明
```

## 当前工程归属

当前 `esp32s3_tirtc_device_monitor_demo` 属于 ESP32-S3 高级示例，后续放入：

```text
esp32-s3/trtc-advanced/
```

它覆盖的能力包括：

- TiRTC 设备端音视频链路
- IPC 查看
- 微信 VoIP
- AI 对讲
- OTA
- 6 位验证码绑定
- 屏幕、触摸、音频、摄像头等板级适配

基础音视频通话最小示例后续放入：

```text
esp32-s3/trtc-basic-call/
```

## 目录归档规则

| 目录 | 放什么 |
| --- | --- |
| `esp32-s3/trtc-basic-call` | ESP32-S3 上最小 TiRTC 音视频通话示例，依赖最少，便于快速跑通 SDK |
| `esp32-s3/trtc-advanced` | 当前设备监控示例，包含 IPC、微信 VoIP、AI 对讲、OTA 和完整 UI |
| `esp32-c3` | ESP32-C3 平台示例，按后续硬件能力拆分子目录 |
| `esp32-p4` | ESP32-P4 平台示例，按后续硬件能力拆分子目录 |
| `s10` | 君正 S10 平台示例 |
| `realtek` | Realtek 平台示例 |
| `common` | 确认被两个及以上平台复用的组件、协议适配层、工具脚本或公共说明 |
| `docs` | 跨平台说明、SDK 接入流程、烧录说明、发布流程和问题排查文档 |

## 迁移边界

- 当前阶段先记录公开仓库目标结构，不急着推送代码。
- 现有 ESP32-S3 工程发布前仍在当前目录完成版本收口、编译和打包。
- 真正迁移到 `tirtc-device-example` 时，再把当前工程整体放入 `esp32-s3/trtc-advanced/`，避免现在移动源码影响本地构建。
- `release_assets` 后续跟随具体示例目录走，例如 `esp32-s3/trtc-advanced/release_assets/`。
- 敏感配置、真实设备密钥、个人联系人标识和私有服务端密钥不能进入公开仓库。

## 后续迁移检查项

1. 确认当前工程在原路径完整编译通过。
2. 将工程整体迁移到 `esp32-s3/trtc-advanced/`。
3. 修正相对路径、文档链接、脚本路径和发布资产路径。
4. 在新目录重新执行一次完整构建。
5. 检查公开仓库里只包含可公开示例、必要文档和当前发布资产。
6. 确认 `README.md`、烧录说明、OTA 说明和 TiRTC 配置说明全部指向新仓库结构。
