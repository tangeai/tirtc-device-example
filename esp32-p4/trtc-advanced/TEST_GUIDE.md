# TiRTC ESP32-P4 Device Monitor Demo 验证说明

本文档用于验证 ESP32-P4 设备监控端示例的构建、启动、绑定、TiRTC 音视频和退出释放流程。

## 准备

- Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 开发板。
- ESP-IDF 5.5.4。
- 可用 Wi-Fi 网络。
- 可触发 TiRTC 呼入的 Web 或移动端。
- 当前工程路径：`%USERPROFILE%\Desktop\demo_p4`。

## 构建

```powershell
cd $env:USERPROFILE\Desktop\demo_p4
cmd /c "C:\esp\v5.5.4\esp-idf\export.bat >nul && idf.py reconfigure build"
```

通过标准：

- `Project build complete`。
- app bin 小于当前 OTA app 分区。
- `firmware/tirtc_esp32p4_wifi_link_demo.bin` 被更新。

## 烧录

```powershell
cd $env:USERPROFILE\Desktop\demo_p4
cmd /c "C:\esp\v5.5.4\esp-idf\export.bat >nul && idf.py -p COMx flash monitor"
```

## 启动验证

串口日志应能看到：

```text
display ready: physical=320x480 ui=480x320 rotation=3
touch input ready
system ready: ESP32-P4 TiRTC dashboard
```

界面应进入横屏首页，触摸点击和返回键位置正确。

## 联网和绑定

通过标准：

- 日志出现 `wifi connected` 和设备 IP。
- SNTP 校时成功。
- 绑定页显示 6 位验证码。
- 绑定完成后，IPC 查看页显示设备 ID。
- 设备在线服务能获取 token 并进入在线状态。

## TiRTC 验证

通过标准：

```text
rtc sdk init stage: TiRtcInit done
rtc listen start stage: TiRtcStart accepted
rtc system started
```

远端呼入后：

```text
rtc connected
peer_connection state change
```

断开或返回首页后，状态快照应回到：

```text
camera=0 rtc=0 fps=0.0 bitrate=0kbps
```

## 视频验证

视频链路使用摄像头 YUV420 输入和 P4 H264 硬编上行。默认目标为 `1920x1080@20fps`、`6Mbps`。

关键日志：

```text
H264 encoder ready
camera pipeline first upstream frame
camera pipeline stats: target=1920x1080@20
local video tx stats
```

观察点：

- Web 或移动端能看到实时画面。
- 周期性 stats 中 `fps`、`bitrate` 与目标接近。
- 不持续出现 backpressure、send buffer stale、encoder failed。
- 断线后 UI 顶部帧率、码率、分辨率清零。

## 音频验证

本机麦克风上行：

```text
mic capture level
local audio tx
```

远端音频播放：

```text
remote audio accepted
speaker playback path ready
```

观察点：

- 本机说话远端能听到。
- 远端说话设备端扬声器能播放。
- 视频启动时音频可以延后进入，但不能长期堆积。

## UI 验证

- 首页卡片点击准确。
- IPC 查看页二维码足够大，底部显示设备 ID。
- 顶部显示分辨率、帧率、码率。
- 返回键可退出 IPC 查看页。
- 返回首页后摄像头、TiRTC、音频资源释放。

## 弱网和策略验证

默认构建关闭自动弱网降级：

```text
CONFIG_APP_RTC_VIDEO_AUTO_ADAPT_ENABLE is not set
```

手动调节接口在 `main/media/media_governor.*`，后续由业务层或 TiRTC 弱网回调触发。默认演示不应自动降分辨率、降帧率或降码率。

## 发布前检查

- `git diff --check` 通过。
- `idf.py reconfigure build` 通过。
- `release_assets/web-flash/v1.0.2/SHA256SUMS.txt` 存在。
- 若生成完整镜像，`release_assets/web-install/v1.0.2/SHA256SUMS.txt` 存在。
- 文档不包含真实密钥、token、个人账号或临时调试叙事。
