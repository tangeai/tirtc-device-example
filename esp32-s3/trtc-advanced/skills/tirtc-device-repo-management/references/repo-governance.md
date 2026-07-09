# 仓库治理参考

## 目标公开仓结构

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

## 当前工程归档

当前 ESP32-S3 设备监控工程归到：

```text
esp32-s3/trtc-advanced/
```

它不是最小 SDK 示例，而是高级示例，包含：

- IPC 查看
- 设备间 TiRTC 呼叫
- 微信 VoIP
- AI 对讲
- OTA
- 6 位验证码绑定
- LVGL UI
- 屏幕、触摸、音频、摄像头适配

未来最小 SDK 跑通示例归到：

```text
esp32-s3/trtc-basic-call/
```

## 迁移策略

迁移分两步，不要混在一次提交里：

1. 当前路径内完成版本发布、文档收口、资产清理和构建验证。
2. 另起一次目录迁移提交，把工程整体放入 `esp32-s3/trtc-advanced/`，再修正路径并重新构建。

## 发布资产规则

- 当前版本只保留当前版本的 bin/zip/checksum。
- ESP Launchpad 推荐使用 `web-install` 下的 0x0 完整合一 bin。
- 手动多地址包只作为维护者排障备用。
- 不再推荐自建网页烧录页。

## 敏感信息规则

公开仓不能包含：

- 真实 `device_id/device_key`
- 长期有效 token
- 私有服务器密钥
- 个人联系人 openid
- 私有 Wi-Fi SSID/password
- 内部临时调试日志中暴露的业务凭证
