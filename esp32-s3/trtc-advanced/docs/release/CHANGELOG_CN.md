# 修改日志

## 0.7.5 - 2026-07-24

### 功能

- 对齐 ThingConnect 当前服务发现、设备绑定、正式 MQTT 和设备在线流程。
- 增加设备间互呼、云端联系人和异常房间恢复。
- 对齐 H5 实时、AI 对讲、微信 VoIP 和设备互呼四类会话。
- 微信联系人接口切换为 `GET /v1/voip/device/contacts`。
- 更新主界面翻页、设备二维码、联系人和呼叫交互。

### 架构

- 应用层统一仲裁前台媒体会话，协议回调不直接递归释放硬件。
- 设备 MQTT 长连接按业务 `channel` 分发消息，绑定 MQTT 和正式 MQTT 生命周期分离。
- NVS 写入、身份代次和旧异步结果增加边界检查。
- UI、application、services、protocols 和 drivers 的资源所有权进一步收口。

### 文档与发布

- 文档结构和四类功能说明对齐
  [tirtc-server-example](https://github.com/tangeai/tirtc-server-example)。
- 首次体验改为“Release 下载 -> 0x0 烧录 -> Wi-Fi -> 6 位码 -> H5 出图出声”。
- 固件 `.bin/.zip` 不再进入 Git，正式资产改由 GitHub Releases 保存。
- 重新生成 0x0 完整镜像、OTA app、维护者包、SHA256 和 manifest。

### 构建

| 项目 | 结果 |
| --- | --- |
| `idf.py build` | 通过 |
| app 大小 | `7291120` bytes |
| app SHA-256 | `E813347914D70D284D82E3AB798E54A6292530CE97A20C30FE4EBA58FE32BFFC` |
| 完整镜像大小 | `16777216` bytes |
| 完整镜像 SHA-256 | `81AA9FE1E1CC0E8A6F428A1D675BE78DA56558F09BD5C45CAD819C46ADA95B46` |
| 真机回归 | 待执行 |
