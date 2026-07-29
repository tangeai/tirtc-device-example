# G32S10X 完整 TiRTC 设备应用

本目录提供君正 G32S10X 与 FreeRTOS 上的完整设备应用源码。APP 版本为 `0.1.1`，
集成 TiRTC SDK `2.2.1`。

## 芯片与应用特性

- G32S10X RISC-V、SFC NAND 与君正 IM SDK `v0.4.0`。
- ATBM WiFi、lwIP、RTC/NTP 和 mbedTLS 网络基础设施。
- LVGL 设备界面、LCD、GC0308 DVP 摄像头、JPEG 视频与设备音频链路。
- TiRTC 设备监控、设备呼叫、微信 VoIP 与 AI 对讲业务入口。
- 可复现的发布 defconfig、SDK 静态清单和平台覆盖文件。

## 准备环境

构建在 Ubuntu/WSL 中完成。请从授权渠道准备：

- 君正 IM SDK `v0.4.0`，并定位到其中的 `im_sdk/opensource/freertos`。
- `riscv32-ingenic-g32s10-elf-tools-r1.0.5` 工具链。
- Windows PowerShell 5.1 或更高版本；仅使用 Linux 时也可直接调用 Shell 入口。

构建脚本不包含本机路径。PowerShell 入口读取以下环境变量，也接受同名参数：

```powershell
$env:G32_SDK_ROOT = "<WSL_SDK_ROOT>/im_sdk/opensource/freertos"
$env:G32_TOOLCHAIN_BIN = "<WSL_TOOLCHAIN_ROOT>/bin"
```

以上路径是 WSL 内路径，请替换为本机实际安装位置。

## 构建

在本目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Test-ReleaseSource.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Release.ps1
```

也可以显式传入路径：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Release.ps1 `
  -G32SdkRoot "<WSL_SDK_ROOT>/im_sdk/opensource/freertos" `
  -G32ToolchainBin "<WSL_TOOLCHAIN_ROOT>/bin"
```

Linux/WSL 直接入口：

```bash
export G32_SDK_ROOT="<WSL_SDK_ROOT>/im_sdk/opensource/freertos"
export G32_TOOLCHAIN_BIN="<WSL_TOOLCHAIN_ROOT>/bin"
bash scripts/build_release.sh all
```

脚本先检查公开源码与 SDK 哈希，再把仓内覆盖层安装到指定 SDK，应用
`g32s10x_tirtc_app_release_defconfig` 并执行干净构建。输出统一写入本目录的
`building/`，包括 `zero.elf`、`zero.bin`、`rtos-with-spl.bin`、
`fs.yaffs2`、`data.yaffs2`、构建日志、`SHA256SUMS.txt` 和构建元数据。
`building/` 不进入 Git。

## 使用君正 Cloner 烧录

1. 使用 IM SDK 配套、且与目标板存储布局一致的君正 Cloner 和板级配置。
2. 在 Cloner 中选择目标板的 SFC NAND 配置，不要根据其他板卡资料猜测分区地址。
3. 从同一次构建的 `building/` 选择 `rtos-with-spl.bin`、`fs.yaffs2` 和
   `data.yaffs2`，并保持它们与板级 Cloner 配置的三个 policy 一一对应。
4. 点击 Cloner 的 `Start` 后，让设备进入 USB 烧录模式；Windows 设备列表应出现
   `VID_A108`。
5. 以本次会话新增的 `policy0/1/2 write ret: ok` 和
   `all policy completed` 作为写入完成依据，再复位设备并查看 UART 启动日志。

Cloner 配置属于具体硬件交付资料，本仓不附带通用配置。更换 Flash 型号、分区布局或
板级引脚后，应先由硬件供应方确认配置，再执行烧录。

## 目录

- `app/`: 应用业务、UI、图片、字库与媒体适配。
- `sdk/`: TiRTC 头文件、G32 静态库和可核验的 SDK 清单。
- `integration/package/`: 君正 package/Kconfig 集成文件。
- `integration/vendor_overrides/`: APP 所需的平台覆盖文件。
- `integration/configs/`: 发布 defconfig；网页抓屏调试默认关闭。
- `scripts/`: 源码检查、覆盖层安装和通用构建入口。
- `tools/`: 资源生成与可选的设备开发工具。

代码来源和静态库哈希见 [SOURCE_PROVENANCE.md](SOURCE_PROVENANCE.md)。
