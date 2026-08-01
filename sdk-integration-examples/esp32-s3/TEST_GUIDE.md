# TiRTC ESP32-S3 Demo 1.2.0 测试说明

本指南把 SDK 身份、编译、烧录、上线、连接、音视频和资源稳定性分开取证。
任一前置阶段通过，都不能替代后续阶段。

## 1. 前置条件

- ESP32-S3 开发板，带 PSRAM。
- Windows ESP-IDF 5.5.4 环境。
- 可访问公网的 2.4 GHz Wi-Fi。
- 本机 `device_id`、device secret 和首次绑定的稳定 `client_id`。
- 主动连接时需要目标 `device_id`，以及业务服务端或 TiRTC DevTools 签发的短时
  一次性 token。
- Web 播放端、官方客户端或第二台设备，用于验证真实收流。

应用级 `AccessKeyId/SecretKeyId` 不得下发到设备。两台板也不需要互存对端 device
secret；主动连接设备只消费最终 token。

## 2. SDK 身份

在工程根目录执行：

```powershell
Get-FileHash -Algorithm SHA256 components\tirtc_sdk\lib\esp32s3\libTiRTC.a
Get-Content components\tirtc_sdk\manifest\build-info.json
Get-Content components\tirtc_sdk\manifest\build-contract.env
```

预期：

```text
SDK version: 2.2.1
commit: 3a33bf4ae51b3ab9eb246648adb274d0fae32ebf
lib size: 4,908,112 bytes
SHA256: dc9f869194584fd43fe091f887342d170a1422e657ce1fc2a144c90a3ade1f8e
ESP-IDF: 5.5.4
toolchain: xtensa-esp32s3-elf-gcc-14.2.0_20260121
FreeRTOS tick: 1000 Hz
transport: KCP / noSCTP
TLS: disabled, HTTP-only
```

哈希不一致时停止测试，不要继续用文档版本号推断库身份。

## 3. 本地配置

```powershell
Copy-Item main\local_config.example.h main\local_config.h
```

在 `main/local_config.h` 中填写 Wi-Fi、本机设备身份和稳定 `client_id`。需要主动
连接时，再写入目标设备 ID 和当次一次性 token。

规则：

- `client_id` 为 1..64 个可打印 ASCII 字符。
- 同一 `device_id` 必须复用首次成功绑定的 `client_id`。
- `TIRTC_SERVICE_ENDPOINT` 正常情况下留空。
- 本 SDK 是 HTTP-only，不能设置 `https://`。
- `main/local_config.h` 必须保持 Git ignored。
- 串口日志不得出现 device secret、应用签名密钥或 token。

`main/local_config.h` 只用于开发联调，其中的值会进入固件二进制，不是量产安全
存储。当前 token 是编译期配置；示例会在 SPIFFS 双槽持久化已预留 token 的
SHA-256 指纹、序号和 CRC，拒绝最近两个有效槽中已记录的值，一次写入中断也
不会破坏上一份有效记录。这是开发联调防误用，服务端的一次性 token 校验仍是
权威边界。重新签发后需要替换该值并重新编译烧录。擦除或重刷 `storage` 分区会
清除本地指纹，之后仍必须使用新 token。量产接入应使用受控凭证注入和运行时
token provider。

双槽标记在调用 `TiRtcConnect()` 前写入，因此“本地预留成功”“SDK 同步返回 0”
和“异步连接成功”是三层独立证据，不能互相替代。

确认忽略状态：

```powershell
git check-ignore -v main\local_config.h
```

## 4. 全新构建

不要复用指向旧 checkout 的 `build/`：

```powershell
. "$env:IDF_PATH\export.ps1"
idf.py -B build-verify reconfigure
idf.py -B build-verify build
```

核对生成配置：

```powershell
Select-String -Path build-verify\config\sdkconfig.h -Pattern `
  'CONFIG_FREERTOS_HZ|CONFIG_LWIP_TCPIP_TASK_STACK_SIZE|CONFIG_SPIRAM'
```

预期至少为：

```text
CONFIG_FREERTOS_HZ=1000
CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096
CONFIG_SPIRAM=y
```

并确认 trace、stats、runtime stats 均未启用。记录：

```powershell
Get-Item build-verify\tirtc_esp32s3_wifi_link_demo.bin
Get-FileHash -Algorithm SHA256 build-verify\tirtc_esp32s3_wifi_link_demo.bin
```

## 5. 烧录与上线

使用 Chrome 或 Edge 打开
[Espressif ESP Tool](https://espressif.github.io/esptool-js/)，按
`build-verify/flasher_args.json` 把同一次构建生成的全部 BIN 和地址逐项加入并烧录。
完成后复位设备，再用 `idf.py -p COMx monitor` 查看串口。

必须烧录清单中的完整分片，确保应用、分区表和 `storage` SPIFFS 测试媒体来自同一次
构建。重写 `storage` 会清空本地 token 标记；烧录后的首次
主动连接必须使用新签发的 token。

启动检查点：

```text
Wi-Fi 已连接
系统时间同步完成
TiRTC 版本: 2.2.1
TiRTC BuildInfo: {"tirtc":"v2.2.1","commit":"3a33bf4ae51b",...}
本地测试媒体已就绪
TiRTC 运行策略: max_connections=1 network=Wi-Fi connect_cache=1
TiRTC 启动请求已提交
TiRTC 已上线
```

`TiRtcStart()` 返回 0 之后，只有收到 `TIRTC_EVENT_SYS_STARTED` 才算上线。

## 6. 主动连接

1. 确认目标设备已经上线。
2. 给本机配置与目标 `remote_id` 匹配的一次性 token。
3. 按 BOOT。
4. 首次连接预期缓存返回 `TIRTC_E_CACHE_EXPIRED`，随后使用新 token。
5. 成功连接同一目标后断开，再按 BOOT，检查缓存连接。

首次连接预期：

```text
主动连接先尝试复用 SDK 连接参数缓存
连接参数缓存未命中或已过期，改用新的一次性 token
使用一次性 token 发起主动连接
主动连接成功
```

缓存有效时预期：

```text
主动连接先尝试复用 SDK 连接参数缓存
缓存连接请求已提交，最终结果以异步回调为准
主动连接成功
```

不要重复使用已消费的一次性 token。缓存失效后必须重新签发。

若要验证本地双槽防误用，需要保留 `storage`，制造缓存失效条件后分别检查：

1. 最近两个已预留 token 都会在调用 SDK 前被本地拒绝。
2. 第三个新 token 可写入较旧槽，并进入 SDK 同步提交阶段。
3. 写入中断故障注入后，至少一个旧槽仍可通过 magic、序号和 CRC 校验。

本地双槽只保存最近两项，也不防止分区被擦除或篡改；服务端一次性校验始终是
权威边界。

DevTools 最小命令和登录流程见
[官方设备实时音视频示例](https://docs.tange.ai/products/tirtc/get-started/view-device-live-av.html)：

```text
tirtc-devtools-cli token issue <remote_id>
```

## 7. 入站连接与音视频

远端连接本机并订阅：

- 视频 stream `11`，首个可发送帧必须为 H264 关键帧。
- 音频 stream `10`，PCMA 8 kHz A-law。

预期：

```text
TiRTC 收到远端连接
[CTRL][RX] 对端订阅本机视频 stream=11
[TX][video] 发流开始
[CTRL][RX] 对端订阅本机音频 stream=10
[TX][audio] 发流开始
```

需要在远端实际看到视频并听到或分析到音频。仅有 `[TX]` 日志不能证明远端播放。

发送缓冲繁忙时允许丢帧，但应看到示例恢复关键帧，缓冲不能长期单调增长。

## 8. 分流退订与重订

1. 同时订阅音频和视频。
2. 只取消视频 stream `11`。
3. 确认 H264 任务停止，PCMA 继续发送。
4. 重订视频，确认第一段有效视频从关键帧恢复。
5. 只取消音频 stream `10`。
6. 确认 PCMA 任务停止，视频继续发送。
7. 重订音频，确认发送恢复。

对应日志：

```text
[CTRL][RX] 对端取消订阅本机视频 stream=11
[TX][video] 本机 H264 发送任务已停止
[CTRL][RX] 对端取消订阅本机音频 stream=10
[TX][audio] 本机 PCMA 发送任务已停止
```

## 9. 断开、停止与资源

连接状态下按 BOOT，确认两端进入断开回调，音视频任务都停止。至少执行 20 次
连接/断开循环。

连接仍在发送媒体时再发起第二条入站或主动连接，确认单连接策略拒绝新句柄，原
连接不会被强行替换；同时触发连接错误与 BOOT 断开，确认日志中每个句柄最多只有
一次应用侧 `TiRtcDisconnect()`，且没有失效句柄调用。

如业务集成调用 `tirtc_deinit()`，必须看到：

```text
TiRTC 已停止
TiRTC 资源已释放
```

出现“尚未收到 `TIRTC_EVENT_SYS_STOPPED`”时，示例会保留 SDK 资源，不会提前
`TiRtcUninit()`。需要保存完整日志定位，不能把超时当作成功停止。

在首轮和第 20 轮记录：

- internal RAM free / minimum free / largest block
- PSRAM free / minimum free / largest block
- SDK 任务是否残留
- 再次启动后是否仍能连接和收发

先在正式 manifest 的符号清单中静态确认扩展任务接口：

```powershell
Select-String components\tirtc_sdk\manifest\symbols-undefined-by-object.txt `
  -Pattern 'xTaskCreatePinnedToCoreWithCaps'
```

该静态门禁证明库引用了目标接口；板端循环用于进一步证明运行稳定性和内存趋势，
单次构建不能替代这部分运行证据。

## 10. 常见错误

`-40014 TIRTC_E_NO_SECRET_KEY`

检查是否在 `TiRtcStart()` 前设置了 `TIRTC_OPT_DEVICE_SECRET_KEY`。

服务端 `X-Tg-Client-Id is required` 或身份冲突

检查 `client_id` 是否有效，并确认已绑定设备复用了首次值。

`TIRTC_E_CACHE_EXPIRED`

这是缓存未命中/过期，不是最终连接失败；获取与目标 scope 一致的新 token。

HTTPS endpoint 参数错误

本正式包为 HTTP-only。清空 endpoint 使用 SDK 默认入口，或使用正式包明确支持的
部署地址。

缺少 H264/PCMA 文件

按本次构建的 `flasher_args.json` 在 Espressif ESP Tool 中重写全部分片，不要只烧录 app bin。

异常重定向事件

立即停止信任当前 endpoint，检查 DNS、代理、热点认证页和中间人风险。

## 11. 留存证据

- SDK 文件哈希、manifest 和 BuildInfo。
- 构建命令、完整成功结尾、固件大小和 SHA256。
- 烧录命令、串口启动到 `SYS_STARTED` 的完整日志。
- 主动/入站连接两端日志。
- 远端实际播放或协议抓取证据。
- 音视频分别退订与重订日志。
- 20 次循环及前后内存数据。
- 脱敏扫描结果。

分享日志前先脱敏；任何 token、device secret 或应用签名密钥都必须删除。
