# 版本信息

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.9.7` |
| 源标签 | `v1.9.7` |
| 源 Tag object | `73f6a3b18ac4ad5e95f813c6eba4f31907961be6` |
| 源提交 | `58c2d152a179fa382511f4217fb84858072ffbb0` |
| 源 tree | `206b2373a389fab39ece1893f23f1ddbf0e66dfe` |
| 比较基线 | `v1.9.6` / `cdb5d7bec9c955227a1259281686e30377332fb3` |
| 公开代码导入提交 | `a3c6d4c5bdecb215f8c8ebec554424de77ed193b` |
| 公开 repository tree | `7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a` |
| 公开项目 tree | `ac13d559aae8f74e92afbf585fe79c5da02aa324` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.7` |
| 发布日期 | `2026-08-28` |
| 芯片 | `ESP32-S3` |
| 开发板 | `LCKFB-SZPI-ESP32-S3-VA` |
| Flash / PSRAM | `16 MB / 8 MB` |
| ESP-IDF | `5.5.4` |
| 工具链 | `xtensa-esp32s3-elf 14.2.0_20260121` |
| TiRTC SDK | `2.3.0 mini` 基线 + active-connect `db7290f` + HTTPS 认证 `13e34c3` |
| TiRTC BuildInfo | `v2.3.0-db7290f` |
| TiRTC BuildInfo commit | `db7290f2404b15f2ab8567dd221853ca2ec054ab` |
| HTTPS 认证修复 commit | `13e34c3e3e3dc6776be4713b5c1e3c17bd282766` |
| TGTRP BuildInfo | `tagv1.5.11-41c9a257` |
| `libTiRTC.a` 大小 | `2,127,226` bytes |
| `libTiRTC.a` SHA-256 | `f5eca6c3212a34d00ce5706e864f8112adc8e6b20b92bc175435299caf4b082b` |
| 公开归档处理 | `xtensa-esp32s3-elf-strip --strip-debug`，只移除 SDK 内部调试信息 |
| FreeRTOS tick | `1000 Hz` |
| 服务发现 | `https://ep-open.tangeopen.com/services` |
| MQTT | `mqtts://mqtt-open.tangeopen.com:8883` |
| 设备主动呼叫微信版本类型 | 体验版（`2`） |
| RTC 产品能力 | 双向音频；摄像头仅用于二维码扫描 |
| 设备上行线格式 | `8 kHz / 16 bit / mono / G.711 A-law / 20 ms / 160 bytes` |
| 串口诊断 CLI | 默认开启；量产配置可关闭 |

## 构建契约

- 应用版本由 ESP-IDF 工程版本写入镜像描述信息；正式构建已核对镜像描述信息为 `1.9.7`。
- 两个 OTA app 分区大小均为 `0x770000`，OTA 固件不得超过该上限。
- 后续如需重新生成公开固件，应从公开代码提交的全新目录执行，不复用来源工程或旧版本
  `build/`。
- 正式构建输入与代码导入提交的 repository tree
  `7508d3f00fb436a0d9bfd716fc1fc152f2d0cf8a`、项目 tree
  `ac13d559aae8f74e92afbf585fe79c5da02aa324` 一致。
- 正式构建使用 ESP-IDF `5.5.4`、GCC `14.2.0_20260121` 和 `--no-ccache`，完成
  `1767/1767`，compiler warning、error 和 ICE 均为 `0`。app 大小为 `7,613,216` bytes
  （`0x742b20`），SHA-256 为
  `49349c1e1755ae5265e3ddea6f57166ccf60b90cdca7d1a1a004539f4faf43ff`；app 分区为
  `7,798,784` bytes，分区剩余 `185,568` bytes（`2.38%`）。
- S3 APP 的 app 分区余量较紧。后续增加图片、字体、日志或调试功能时必须重新核对容量，
  不能复用本版本数值。
- TiRTC SDK 的头文件、静态库、组件配置和 `components/tirtc_sdk/VERSION.md` 必须来自同一
  份公开快照，不能只改版本文字。库内 BuildInfo 仍为 `v2.3.0-db7290f`，HTTPS 认证修复
  由 `13e34c3` 提供；两者共同描述当前 SDK 契约。
- 正式完整镜像大小、SHA-256、烧录地址和公开 commit 以同一 Release 的
  `release-manifest.json` 为准。

## 调度与呼叫契约

- UI 只投递接听、拒绝、挂断和音量/增益请求；应用任务执行具体操作。连续音量与增益请求
  可以合并，但必须保留最新值。
- Device Call 的异步 worker 绑定呼叫 generation。generation 变化后，旧 worker 不得提交
  新连接、恢复旧房间或覆盖当前状态。
- 主叫响铃上限为 `45` 秒；进入 `CONNECTING` 后单独执行 `40` 秒 P2P 建连超时。
- 一次 ICE UDP 回调最多处理 `8` 个数据报或 `4 ms`；一次 TGTRP 音频抖动轮询最多释放
  `4` 个有序项，剩余积压在 `1 ms` 后继续。该限制不改变排序和丢包统计契约。

## 传输安全契约

- 服务发现只接受 `HTTPS`，设备业务消息只接受 `MQTTS`。发现结果若返回 `http://` 或
  `mqtt://`，应用会保留安全兜底并拒绝连接（fail closed），不会把 Token、设备密钥或控制
  消息降级到明文链路。
- 应用 HTTP 客户端使用 ESP-IDF 完整证书包，校验证书链和主机名，并禁用自动重定向。
- TiRTC SDK 自有 HTTP 传输同样使用 `MBEDTLS_SSL_VERIFY_REQUIRED`，挂接 ESP-IDF 证书包并
  校验 hostname。证书链、主机名、握手或 verify flags 失败时返回 `CONN_E_SSL`，不得回退 HTTP。
- 公开 `libTiRTC.a` 使用 `--strip-debug` 清除 SDK 内部源码路径和行号级调试信息。归档成员、
  全局符号及已分配代码/只读数据保持一致；此处理不等于重新编译或改写功能实现。

## 已知运行边界

Web IPC 和设备互呼的当前人耳试听仍可感知轻微“沙沙电流声”。线上格式核对和 20,000 次
A-law 编解码自检均未发现异常，但这些证据不能证明底噪来源，根因仍未闭环。`1.9.7` 不把
该现象写成已经修复。TLS 源码、对象、归档和链接检查也不代替目标板上的有效证书成功、
错误证书/主机名失败测试。`1.9.7` 已完成正式干净构建，但没有真机证据；烧录、联网、业务
闭环和音频主观听感继续分别留证。
