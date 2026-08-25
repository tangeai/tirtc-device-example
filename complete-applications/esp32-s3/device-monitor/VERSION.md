# 版本信息

| 项目 | 当前值 |
| --- | --- |
| 应用版本 | `1.9.5` |
| 源标签 | `v1.9.5` |
| 源 Tag object | `318f99c88fa1f8a8a767b0686cbb8f0182b713f4` |
| 源提交 | `45db394cae399967a9c3b882d595cdecb80321be` |
| 源 tree | `4a6760708d4fd2bdb973c4bc77d789d45f2bc2be` |
| 比较基线 | `v1.9.0` / `a64422b0efdebe6c303370effafd52bbf51593d1` |
| 公开代码提交 | `c27914eafab6f700cecf196da48987200cd54d37` |
| 公开 repository tree | `b0b93ddc0c82ae65130ae6dab17ff6ee6dbfc86a` |
| 公开项目 tree | `9fa7f85ce92a0e5fcd27c4a36fabe02609191391` |
| 公开项目 Tag | `esp32-s3-device-monitor-v1.9.5` |
| 发布日期 | `2026-08-25` |
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
| `libTiRTC.a` 大小 | `2,125,366` bytes |
| `libTiRTC.a` SHA-256 | `83556eeee0c6cae45961899a4c5d1255a5d0d33f8e636104a946ce41ff3e20d7` |
| 公开归档处理 | `xtensa-esp32s3-elf-strip --strip-debug`，只移除 SDK 内部调试信息 |
| FreeRTOS tick | `1000 Hz` |
| 服务发现 | `https://ep-open.tangeopen.com/services` |
| MQTT | `mqtts://mqtt-open.tangeopen.com:8883` |
| 设备主动呼叫微信版本类型 | 体验版（`2`） |
| RTC 产品能力 | 双向音频；摄像头仅用于二维码扫描 |
| 设备上行线格式 | `8 kHz / 16 bit / mono / G.711 A-law / 20 ms / 160 bytes` |
| 串口诊断 CLI | 默认开启；量产配置可关闭 |

## 构建契约

- 应用版本由 ESP-IDF 工程版本写入镜像描述信息；本次正式构建必须核对为 `1.9.5`，后续
  重建也必须保持一致。
- 两个 OTA app 分区大小均为 `0x770000`，OTA 固件不得超过该上限。
- 后续如需重新生成公开固件，应从公开代码提交的全新目录执行，不复用来源工程或旧版本
  `build/`。
- 本次正式构建使用的隔离候选与公开代码提交的 repository tree
  `b0b93ddc0c82ae65130ae6dab17ff6ee6dbfc86a`、项目 tree
  `9fa7f85ce92a0e5fcd27c4a36fabe02609191391` 完全一致。
- 正式构建步骤为 `1767/1767`；app 大小为 `7,608,608` bytes，
  SHA-256 为 `51a7599942f06556e33ef4820499885d6213ff15fce0f3ed2f11e38e44146503`，app 分区为 `7798784` bytes，
  分区剩余 `190,176` bytes（`2.44%`）。
- 正式干净构建确认 app 余量为 `190,176` bytes（`2.44%`），已经属于明确容量风险；
  后续增加图片、字体、日志或调试功能时必须重新核对容量。
- TiRTC SDK 的头文件、静态库、组件配置和 `components/tirtc_sdk/VERSION.md` 必须来自同一
  份公开快照，不能只改版本文字。库内 BuildInfo 仍为 `v2.3.0-db7290f`，HTTPS 认证修复
  由 `13e34c3` 提供；两者共同描述当前 SDK 契约。
- 正式完整镜像大小、SHA-256、烧录地址和公开 commit 以同一 Release 的
  `release-manifest.json` 为准。

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
A-law 编解码自检均未发现异常，但这些证据不能证明底噪来源，根因仍未闭环。`1.9.5` 不把
该现象写成已经修复。TLS 源码、对象、归档和链接检查也不代替目标板上的有效证书成功、
错误证书/主机名失败测试；正式构建、烧录、联网、业务闭环和音频主观听感继续分别留证。
