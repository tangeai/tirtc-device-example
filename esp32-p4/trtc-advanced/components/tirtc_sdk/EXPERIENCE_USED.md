# Experience Used

1. Upstream master can be used as a base, but our delivery build needs the local S3/P4 task lifecycle and P4 include fixes reapplied.
2. S3/P4 SDK builds use p2p=kcp and nossl=y so noSCTP/KCP is merged into libTiRTC.a and HTTP is the default service transport.
3. FreeRTOS control objects and task lifecycle must stay owned by FreeRTOS/IDF; large task stacks can use IDF caps APIs for PSRAM.
4. P4 must use its own sdkconfig and riscv32 toolchain, and must include the ESP-IDF 5.5.4 hw_ver1 register path.
