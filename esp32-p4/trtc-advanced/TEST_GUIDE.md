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

视频链路使用摄像头原生 YUV420 输入和 P4 H264 硬编上行。默认平衡档为 `1280x960@20fps`、`4Mbps`。

关键日志：

```text
H264 encoder ready
camera pipeline first upstream frame
camera pipeline stats: target=1280x960@20
local video tx stats
```

观察点：

- Web 或移动端能看到实时画面。
- 周期性 stats 中 `fps`、`bitrate` 与目标接近。
- 背压时允许有界丢帧，但不能由本地发送缓冲保护逻辑主动关闭 TiRTC 连接。
- 不持续出现 encoder failed；连接关闭必须能对应到 TiRTC 回调或明确的用户挂断动作。
- 断线后 UI 顶部帧率、码率、分辨率清零。

## 60 秒性能健康测试

保持一次音视频通话至少 60 秒，将完整串口日志保存为文件，再运行：

```powershell
python tools\analyze_media_performance.py logs\media-60s.txt
```

分析器只读取现有的低频统计，不增加设备端日志和媒体链路负担。它检查持续出帧、实际帧率、码率、丢帧、TiRTC 发送失败、最大帧间隔、DMA 连续块以及严重运行错误。

平衡档验收标准：

- 结果应为 `PASS`；短时无线波动允许为 `WARN`，但发布前不能存在 `FAIL`。
- 摄像头平均帧率不低于目标的 80%，即至少约 16fps。
- TiRTC 视频发送失败率不高于 5%，平均发送帧率不低于 15fps。
- 最大帧间隔不高于 150ms。
- 最小 `dma_largest` 不低于 16KB。
- 不出现 SDIO DMA 内存不足、心跳超时、无效句柄或系统崩溃。

三路 CSI 缓冲是采集、硬编码和回队列并行所需的流水线深度，不再为了节省单帧 PSRAM 将其压缩为两路。后续参数调整以 60 秒测试结果为依据，一次只改变一个变量。

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
- IPC 查看、设备通话、微信 VoIP 和 AI Chat 进入全双工媒体阶段后，AEC 应按当前音频所有者启用；退出后停止处理并保留预热资源。

## P4 设备通话验证

- 进入呼叫页时联系人列表从服务端刷新，不依赖 NVS 缓存。
- 默认本机视频为 `480x320@15fps`、`2Mbps`，远端 H264 解码后以横屏视口显示。
- 远端画面颜色、分区和方向正确，不出现三分屏、绿屏或周期性撕裂。
- 挂断按键一次点击即可进入结束流程，媒体队列和显示资源随后释放。

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
- `release_assets/web-flash/v1.1.0/SHA256SUMS.txt` 存在。
- 若生成完整镜像，`release_assets/web-install/v1.1.0/SHA256SUMS.txt` 存在。
- 文档不包含真实密钥、token、个人账号或临时调试叙事。
