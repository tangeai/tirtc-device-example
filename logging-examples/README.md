# 日志示例

本目录提供面向协议联调和问题定位的 TiRTC 日志示例。它在最小集成基础上增加稳定的串口控制面、
结构化状态日志和敏感字段脱敏，但不引入完整产品 UI 或真实板级音视频外设。

## 当前示例

| 平台 | 示例 | 版本 | TiRTC SDK | 构建环境 | 来源版本 |
| --- | --- | --- | --- | --- | --- |
| ESP32-S3 | [TiRTC 日志示例（AT ThingConnect）](esp32-s3/README.md) | `0.7.0` | `2.2.1` | ESP-IDF `5.5.4` | 独立源码快照 / public commit |

## 适用场景

- 用串口 AT 完成 Wi-Fi、设备绑定、状态查询和业务动作。
- 联调 ThingConnect 服务发现、设备 Report、正式 MQTT 和 Token 更新。
- 观察 AI 对讲、联系人解析、设备呼叫和生命周期交接。
- 使用预录 G711A/H264 素材复现媒体发送，不依赖摄像头、麦克风、屏幕或触摸。
- 通过简洁中文日志定位状态机、竞态和异常恢复问题。

日志中的凭据、Token 和设备身份默认脱敏；项目配置不包含可直接使用的生产凭据。正式构建产物
只随 [GitHub Releases](https://github.com/tangeai/tirtc-device-example/releases) 分发，不进入
Git 历史。
