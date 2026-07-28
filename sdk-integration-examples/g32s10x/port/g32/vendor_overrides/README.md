# G32S10X Vendor Overrides

本目录按相对路径覆盖 Ingenic `im_sdk_v0.4.0`，只适用于本例程锁定的 G32S10X SDK 和
`riscv32-ingenic-r1.0.5` 工具链。覆盖发生在应用同步之后、静态检查和完整编译之前。

覆盖内容分为三类：

1. 最小网络运行时：裁剪未使用的 ATBM、lwIP 功能，并提供 TiRTC 需要的 TLS、随机数和
   newlib 系统调用适配。
2. 基础运行时修复：修正异常处理、栈回溯、时钟、IRQ、MMC、符号表和 mbedTLS 中的真实
   类型、边界及空指针问题。
3. 编译器兼容：君正 r1.0.5 将 `uint32_t` 定义为 `unsigned long`，而旧 SDK 的诊断打印大量
   使用 `%x/%d`。`package/build/build.mk` 只为这些供应商诊断关闭格式告警；例程源码仍由
   `scripts/syntax_check.sh` 使用 `-Wformat=2 -Werror` 检查。

根目录构建脚本会扫描完整 `building/build.log`。任何编译器或链接器 `warning:` 都会让构建
失败，因此不能通过删除静态检查或扩大 `-Wno-*` 范围来绕过问题。

升级原始 SDK 或工具链时，应重新对照上游文件逐项移植这些修复，不能直接假定整份覆盖文件
仍然兼容。
