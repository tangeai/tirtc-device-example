# 君正设备开发控制台

这一目录只服务 G32S10X。它把静态检查、构建、Cloner 准备、烧录提示旋律、
`COM39` 日志、设备屏幕抓取和本机网页控制台组织成一条可复现流程。

## 分层

- `Invoke-G32DevLoop.ps1`：开发流程编排，不直接实现驱动或产品业务。
- `Enter-G32BurnMode.ps1`：播放提示旋律，可选控制已验证接线的 DTR/RTS，并等待 `VID_A108`。
- `Wait-G32FlashResult.ps1`：只接受本次 Cloner 会话新增的 `all policy completed`。
- `-FlashScript`：显式传入板级 Cloner 准备脚本；该脚本和配置由硬件交付方提供。
- `Start-G32DeviceConsole.ps1`：启动长期串口监控和本机 HTTP 服务。
- `server/`：设备状态、BMP 屏幕和串口日志的同源代理。
- `web/`：浏览器里的实时设备屏幕与日志界面。
- 设备固件 `tirtc_screen_debug`：受 `CONFIG_APPLICATION_TIRTC_SCREEN_DEBUG` 控制，端口 8080 提供 `/`、`/screen.bmp` 和 `/api/status`。

产品业务仍由君正原有 WiFi、LVGL、音视频和 TiRTC 应用层负责。屏幕服务只在持有
`jz_lv_mutex` 时复制 framebuffer；BMP 转换和网络发送都在解锁后执行，避免长期占用 UI。
当前开发镜像为网页验收启用了该宏；正式交付若不需要局域网抓屏，应设置为 `n`。

## 开发流程

```powershell
$env:G32_SDK_ROOT = "<WSL_SDK_ROOT>/im_sdk/opensource/freertos"
$env:G32_TOOLCHAIN_BIN = "<WSL_TOOLCHAIN_ROOT>/bin"
.\device-console\Invoke-G32DevLoop.ps1 -Action Preflight
.\device-console\Invoke-G32DevLoop.ps1 -Action Build
.\device-console\Invoke-G32DevLoop.ps1 -Action Flash -ComPort COM39 `
  -FlashScript <board-specific-cloner-script.ps1>
```

`Flash` 会安装同批三镜像配置、启动串口记录并打开 Cloner。当前 Windows Cloner 的
`core.exe -b` 只是 GUI 后端，单独启动不会进入写入状态；必须由 Cloner GUI 发出
`Start` 命令。点击 `Start` 后运行：

```powershell
.\device-console\Invoke-G32DevLoop.ps1 -Action BurnMode -ComPort COM39
.\device-console\Invoke-G32DevLoop.ps1 -Action VerifyFlash
```

听到旋律后，按住 `BOOT`、短按并松开 `RESET`、再松开 `BOOT`。`BurnMode` 只接受
君正 `VID_A108`；`VerifyFlash` 只检查会话启动后追加的 Cloner 日志，旧成功记录不能
冒充本轮结果。三镜像成功时应同时出现 `policy0/1/2 write ret: ok` 和
`all policy completed`。

只有板卡把 USB-UART 的 DTR/RTS 确实接到 BOOT/RESET 后，才能选择
`DtrResetRtsBoot` 或 `RtsResetDtrBoot`。当前默认 `None`，避免在未知接线下误拉外设。

## 屏幕与日志网页

设备串口出现 `[screen_debug] ready ip=<IP>` 后启动本机控制台：

```powershell
.\device-console\Start-G32DeviceConsole.ps1 -DeviceIp 192.168.31.158 -ComPort COM39 -Port 8787 -Restart
.\device-console\Test-G32DeviceConsole.ps1
```

浏览器地址固定为 `http://127.0.0.1:8787/`。网页不把设备端口暴露到局域网之外，
并同时显示真实 `480x854` BMP 与 `building` 中最新的 G32 串口日志。直接设备验收可用：

```powershell
.\device-console\Test-G32ScreenCapture.ps1 -DeviceIp 192.168.31.158
```

测试只有在状态接口返回 `G32S10X`、BMP 为 `480x854`、像素非空且串口日志可读时才通过。

## 固定门禁

1. 先解析检查全部 PowerShell、Python 和 JavaScript 控制代码。
2. 再执行 `strict_check_tirtc.sh` 与 S3 运行 UI 门禁；失败就停止，不进入慢编译。
3. 静态检查通过后才 clean build，三镜像只取唯一 `building` 目录的同一批次。
4. Cloner 点击 `Start` 后才播放旋律，用户只负责实体 BOOT/RESET 动作。
5. 烧录成功必须有本轮 `all policy completed`，启动成功必须有新的 G32 串口签名。
6. `building` 保留预检、构建、Cloner 会话、烧录结果、串口和抓屏证据。
