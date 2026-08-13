# ESP32-P4 TiRTC 日志示例

这个示例面向需要观察 TiRTC、ThingConnect 和设备会话状态的开发者。它用串口 AT 指令完成
配网、绑定、网页查看、AI 对讲和设备互呼，并把内部状态整理成简洁的中文日志。媒体输入来自
工程内的 H264/G711A 测试文件，因此不需要摄像头、麦克风、屏幕或触摸。

同一份 ESP32-P4 固件兼容两种 Wi-Fi 协处理器：ESP32-C6 和 ESP32-C61。P4 通过
ESP-Hosted/SDIO 联网；两种板卡使用相同的数据线，固件会在启动时尝试对应的复位 GPIO。

## 直接烧录

从 [ESP32-P4 日志示例 v0.1.0](https://github.com/tangeai/tirtc-device-example/releases/tag/esp32-p4-logging-v0.1.0)
下载以下三个文件：

- `esp32p4-tirtc-at-demo-full-v0.1.0.bin`
- `SHA256SUMS.txt`
- `release-manifest.json`

先用 SHA-256 核对固件，再按下面步骤烧录：

1. 关闭正在占用 P4 串口的串口工具。
2. 用 Chrome 或 Edge 打开 [Espressif ESP Tool](https://espressif.github.io/esptool-js/)。
3. 点击 `Connect`，选择 P4 的 CH343 串口，并确认网页识别到 `ESP32-P4`。
4. 点击 `Add File`，选择完整固件，`Flash Address` 填写 `0x0`。
5. 首次使用、跨项目切换或设备状态不确定时，先执行 `Erase Flash`。
6. 点击 `Program`。完成后按开发板 `RESET`。

这个 4 MB 完整镜像会清除原有 NVS、Wi-Fi 和绑定数据。C6/C61 协处理器还需运行与目标芯片
匹配的 ESP-Hosted 从机固件；本 Release 提供的是 P4 主控固件。

## 串口与首次联网

串口设置为 `115200`、`UTF-8`、`CRLF`。把命令中的名称和密码换成自己的 Wi-Fi：

```text
AT+配网="Wi-Fi名称","Wi-Fi密码"
```

设备重启并重新连接串口后发送：

```text
AT+状态
```

看到 `已就绪` 和 `空闲`，说明 P4 已通过 C6/C61 联网并完成平台初始化。

## 绑定与网页查看

发送 `AT+绑定` 获取六位绑定码，然后进入
[开发者平台](https://demo-open.tange-ai.com/) 的“我的设备”完成绑定。设备重启并恢复
`已就绪` 后，在设备列表点击“实时”，即可查看内置 H264/G711A 测试媒体和结构化设备日志。

## 设备互呼与消息

准备两台已联网、已绑定的设备。先在被叫侧查询设备 ID：

```text
AT+设备ID
```

然后在主叫侧发起呼叫，被叫侧接听：

```text
AT+呼叫="被叫设备ID"
AT+接听
```

接通后可以发送文本，完成后挂断：

```text
AT+发消息="你好"
AT+挂断
```

联系人备注、AI 插件呼叫和完整命令列表可通过 `AT+帮助` 查看。AI 插件使用 `call_device`
功能标识，输入参数为字符串 `target`，可填写联系人备注或设备 ID。

## 从源码构建

依赖：

- ESP-IDF `5.5.4`
- `riscv32-esp-elf-gcc 14.2.0_20260121`
- TiRTC SDK `2.3.0`
- ESP-Hosted `2.12.11`
- ESP-WiFi-Remote `1.6.3`

在项目目录执行：

```powershell
. C:\esp\v5.5.4\esp-idf\export.ps1
idf.py -B build --no-ccache reconfigure build
```

项目使用 4 MB 自定义分区：应用从 `0x10000` 开始，SPIFFS 测试媒体从 `0x210000` 开始。
本仓保留了 C6/C61 同包兼容所需的 ESP-Hosted 源码改动；升级该组件时需要重新核对运行时芯片
识别和 GPIO 复位回退逻辑。

## 配置与安全

- Wi-Fi、设备身份和平台凭据由串口流程写入 NVS，不在源码中预置可用值。
- 用户日志对密码、Token、设备密钥和身份字段做脱敏；SDK 详细日志保持关闭。
- `build/`、`sdkconfig`、`managed_components/` 和固件输出不进入 Git 历史。
- TiRTC 静态库和第三方组件保留各自许可与分发条款。

## 验证边界

本版本已完成公开源码筛选、敏感信息扫描、主机侧单元测试、ESP-IDF 干净构建、4 MB 完整镜像
分段校验和发布附件哈希校验。正式发布时没有可用 P4 串口，因此本次未重新执行 P4 烧录、
C6/C61 真机联网、网页音视频或长时间运行验证；这些运行证据应在目标板上继续完成。
