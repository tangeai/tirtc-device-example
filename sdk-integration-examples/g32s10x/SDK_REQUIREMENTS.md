# 君正 SDK 要求

本示例不分发君正供应商 SDK、工具链或烧录工具。已适配的构建契约：

| 项目 | 要求 |
| --- | --- |
| 芯片 | G32S10X |
| OS | FreeRTOS |
| 君正 SDK | `im_sdk_v0.4.0` |
| SDK 固件根目录 | `im_sdk/opensource/freertos` |
| 工具链 | `riscv32-ingenic-g32s10-elf-tools-r1.0.5` |
| 交叉编译前缀 | `riscv32-unknown-elf-` |

供应商 SDK 包含非公开交付边界和 Linux 符号链接，应在 Linux/WSL 原生文件系统中解压。
开发者需要自行确认 SDK 与工具链授权。

执行构建辅助脚本前，先把项目复制到仓库外、不受 Git 管理的私有构建目录，再将副本中
`include/tirtc_link_config.h` 的 Wi-Fi 和 TiRTC 参数改为测试配置。真实凭据和构建输出不要
写回或分享源码工作树。随后通过环境变量传入 SDK 与工具链位置：

```bash
G32_SDK_ROOT="<WSL_SDK_ROOT>/im_sdk/opensource/freertos" \
G32_TOOLCHAIN_BIN="<WSL_TOOLCHAIN_ROOT>/bin" \
bash scripts/build-in-sdk.sh
```

脚本拒绝覆盖已存在的同名应用目录，建议每次使用干净展开的 SDK。
