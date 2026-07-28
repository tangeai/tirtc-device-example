#include <asm/additional.h>
#ifdef __ASSEMBLY__

.extern pxCurrentTCB
.extern xCriticalNesting
.extern pxCriticalNesting
.extern g_exc_stack_frame
.extern os_handler_mode

/* ------------------------------------------------------------
 * 保存AIE向量寄存器
 * ------------------------------------------------------------ */
.macro portasmSAVE_AIE_VECTOR_REGS
#ifdef CONFIG_RISCV1_AIE_VECTOR
    li t1, -portasmVXU_CONTEXT_WORDSIZE
    add sp, sp, t1

    /* 保存VR0-VR7 */
    add t0, sp, x0
    aie.savw vp0, VXU_OFFSET_VR0_VR0(t0), pr0
    aie.savw vp1, VXU_OFFSET_VR1_VR0(t0), pr0
    aie.savw vp2, VXU_OFFSET_VR2_VR0(t0), pr0
    aie.savw vp3, VXU_OFFSET_VR3_VR0(t0), pr0
    aie.savw vp4, VXU_OFFSET_VR4_VR0(t0), pr0
    aie.savw vp5, VXU_OFFSET_VR5_VR0(t0), pr0
    aie.savw vp6, VXU_OFFSET_VR6_VR0(t0), pr0
    aie.savw vp7, VXU_OFFSET_VR7_VR0(t0), pr0

    /* 保存VR8-VR15 */
    li t1, VXU_OFFSET_VR8_VR0
    add t0, sp, t1
    aie.savw vp8,  VXU_OFFSET_VR8_VR8(t0), pr0
    aie.savw vp9,  VXU_OFFSET_VR9_VR8(t0), pr0
    aie.savw vp10, VXU_OFFSET_VR10_VR8(t0), pr0
    aie.savw vp11, VXU_OFFSET_VR11_VR8(t0), pr0
    aie.savw vp12, VXU_OFFSET_VR12_VR8(t0), pr0
    aie.savw vp13, VXU_OFFSET_VR13_VR8(t0), pr0
    aie.savw vp14, VXU_OFFSET_VR14_VR8(t0), pr0
    aie.savw vp15, VXU_OFFSET_VR15_VR8(t0), pr0

    /* 保存VR16-VR23 */
    li t1, VXU_OFFSET_VR16_VR0
    add t0, sp, t1
    aie.savw vp16, VXU_OFFSET_VR16_VR16(t0), pr0
    aie.savw vp17, VXU_OFFSET_VR17_VR16(t0), pr0
    aie.savw vp18, VXU_OFFSET_VR18_VR16(t0), pr0
    aie.savw vp19, VXU_OFFSET_VR19_VR16(t0), pr0
    aie.savw vp20, VXU_OFFSET_VR20_VR16(t0), pr0
    aie.savw vp21, VXU_OFFSET_VR21_VR16(t0), pr0
    aie.savw vp22, VXU_OFFSET_VR22_VR16(t0), pr0
    aie.savw vp23, VXU_OFFSET_VR23_VR16(t0), pr0

    /* 保存VR24-VR31 */
    li t1, VXU_OFFSET_VR24_VR0
    add t0, sp, t1
    aie.savw vp24, VXU_OFFSET_VR24_VR24(t0), pr0
    aie.savw vp25, VXU_OFFSET_VR25_VR24(t0), pr0
    aie.savw vp26, VXU_OFFSET_VR26_VR24(t0), pr0
    aie.savw vp27, VXU_OFFSET_VR27_VR24(t0), pr0
    aie.savw vp28, VXU_OFFSET_VR28_VR24(t0), pr0
    aie.savw vp29, VXU_OFFSET_VR29_VR24(t0), pr0
    aie.savw vp30, VXU_OFFSET_VR30_VR24(t0), pr0
    aie.savw vp31, VXU_OFFSET_VR31_VR24(t0), pr0

    /* 保存VS寄存器（向量和寄存器） */
    li t1, VXU_OFFSET_VS0_VR0
    add t0, sp, t1
    aie.mfsum vp0, vs0, pr0
    aie.savw  vp0, VXU_OFFSET_VS0_VS0(t0), pr0
    aie.mfsum vp0, vs1, pr0
    aie.savw  vp0, VXU_OFFSET_VS1_VS0(t0), pr0
    aie.mfsum vp0, vs2, pr0
    aie.savw  vp0, VXU_OFFSET_VS2_VS0(t0), pr0
    aie.mfsum vp0, vs3, pr0
    aie.savw  vp0, VXU_OFFSET_VS3_VS0(t0), pr0

    /* 保存VM寄存器（向量掩码/谓词寄存器） */
    li t1, VXU_OFFSET_VM0_VR0
    add t0, sp, t1
    aie.psd pr1, VXU_OFFSET_VM1_VM0(t0)
    aie.psd pr2, VXU_OFFSET_VM2_VM0(t0)
    aie.psd pr3, VXU_OFFSET_VM3_VM0(t0)
#endif /* CONFIG_RISCV1_AIE_VECTOR */
.endm

/* ------------------------------------------------------------
 * 恢复AIE向量寄存器
 * ------------------------------------------------------------ */
.macro portasmRESTORE_AIE_VECTOR_REGS
#ifdef CONFIG_RISCV1_AIE_VECTOR
    /* 恢复VM寄存器 */
    li t1, VXU_OFFSET_VM0_VR0
    add t0, sp, t1
    aie.pld pr1, VXU_OFFSET_VM1_VM0(t0)
    aie.pld pr2, VXU_OFFSET_VM2_VM0(t0)
    aie.pld pr3, VXU_OFFSET_VM3_VM0(t0)

    /* 恢复VS寄存器 */
    li t1, VXU_OFFSET_VS0_VR0
    add t0, sp, t1
    aie.lavw  vp0, VXU_OFFSET_VS0_VS0(t0), pr0
    aie.mtsum vs0, vp0, pr0
    aie.lavw  vp0, VXU_OFFSET_VS1_VS0(t0), pr0
    aie.mtsum vs1, vp0, pr0
    aie.lavw  vp0, VXU_OFFSET_VS2_VS0(t0), pr0
    aie.mtsum vs2, vp0, pr0
    aie.lavw  vp0, VXU_OFFSET_VS3_VS0(t0), pr0
    aie.mtsum vs3, vp0, pr0

    /* 恢复VR0-VR7 */
    add t0, sp, x0
    aie.lavw vp0, VXU_OFFSET_VR0_VR0(t0), pr0
    aie.lavw vp1, VXU_OFFSET_VR1_VR0(t0), pr0
    aie.lavw vp2, VXU_OFFSET_VR2_VR0(t0), pr0
    aie.lavw vp3, VXU_OFFSET_VR3_VR0(t0), pr0
    aie.lavw vp4, VXU_OFFSET_VR4_VR0(t0), pr0
    aie.lavw vp5, VXU_OFFSET_VR5_VR0(t0), pr0
    aie.lavw vp6, VXU_OFFSET_VR6_VR0(t0), pr0
    aie.lavw vp7, VXU_OFFSET_VR7_VR0(t0), pr0

    /* 恢复VR8-VR15 */
    li t1, VXU_OFFSET_VR8_VR0
    add t0, sp, t1
    aie.lavw vp8,  VXU_OFFSET_VR8_VR8(t0), pr0
    aie.lavw vp9,  VXU_OFFSET_VR9_VR8(t0), pr0
    aie.lavw vp10, VXU_OFFSET_VR10_VR8(t0), pr0
    aie.lavw vp11, VXU_OFFSET_VR11_VR8(t0), pr0
    aie.lavw vp12, VXU_OFFSET_VR12_VR8(t0), pr0
    aie.lavw vp13, VXU_OFFSET_VR13_VR8(t0), pr0
    aie.lavw vp14, VXU_OFFSET_VR14_VR8(t0), pr0
    aie.lavw vp15, VXU_OFFSET_VR15_VR8(t0), pr0

    /* 恢复VR16-VR23 */
    li t1, VXU_OFFSET_VR16_VR0
    add t0, sp, t1
    aie.lavw vp16, VXU_OFFSET_VR16_VR16(t0), pr0
    aie.lavw vp17, VXU_OFFSET_VR17_VR16(t0), pr0
    aie.lavw vp18, VXU_OFFSET_VR18_VR16(t0), pr0
    aie.lavw vp19, VXU_OFFSET_VR19_VR16(t0), pr0
    aie.lavw vp20, VXU_OFFSET_VR20_VR16(t0), pr0
    aie.lavw vp21, VXU_OFFSET_VR21_VR16(t0), pr0
    aie.lavw vp22, VXU_OFFSET_VR22_VR16(t0), pr0
    aie.lavw vp23, VXU_OFFSET_VR23_VR16(t0), pr0

    /* 恢复VR24-VR31 */
    li t1, VXU_OFFSET_VR24_VR0
    add t0, sp, t1
    aie.lavw vp24, VXU_OFFSET_VR24_VR24(t0), pr0
    aie.lavw vp25, VXU_OFFSET_VR25_VR24(t0), pr0
    aie.lavw vp26, VXU_OFFSET_VR26_VR24(t0), pr0
    aie.lavw vp27, VXU_OFFSET_VR27_VR24(t0), pr0
    aie.lavw vp28, VXU_OFFSET_VR28_VR24(t0), pr0
    aie.lavw vp29, VXU_OFFSET_VR29_VR24(t0), pr0
    aie.lavw vp30, VXU_OFFSET_VR30_VR24(t0), pr0
    aie.lavw vp31, VXU_OFFSET_VR31_VR24(t0), pr0

    /* 恢复栈指针 */
    li t1, portasmVXU_CONTEXT_WORDSIZE
    add sp, sp, t1
#endif /* CONFIG_RISCV1_AIE_VECTOR */
.endm

/* ------------------------------------------------------------
 * 保存浮点寄存器
 * ------------------------------------------------------------ */
.macro portSAVE_FpuReg
#ifdef CONFIG_RISCV1_FPU
    /* 检查浮点状态是否为脏（dirty） */
    li t0, MSTATUS_FS
    /* csrr t1, mstatus */
    /* and t0, t1, t0 */
    /* li t2, MSTATUS_FS_OFF */
    /* beq t2, t0, 1f */  /* 如果FS为OFF，跳过保存 */


    /* 为浮点寄存器分配栈空间 */
    addi sp, sp, -portasmFPU_CONTEXT_WORDSIZE

    /* 保存所有32个浮点寄存器 */
    fsw f0,  PORT_CONTEXT_fpuOFFSET(0)(sp)   /* ft0 */
    fsw f1,  PORT_CONTEXT_fpuOFFSET(1)(sp)   /* ft1 */
    fsw f2,  PORT_CONTEXT_fpuOFFSET(2)(sp)   /* ft2 */
    fsw f3,  PORT_CONTEXT_fpuOFFSET(3)(sp)   /* ft3 */
    fsw f4,  PORT_CONTEXT_fpuOFFSET(4)(sp)   /* ft4 */
    fsw f5,  PORT_CONTEXT_fpuOFFSET(5)(sp)   /* ft5 */
    fsw f6,  PORT_CONTEXT_fpuOFFSET(6)(sp)   /* ft6 */
    fsw f7,  PORT_CONTEXT_fpuOFFSET(7)(sp)   /* ft7 */

    fsw f8,  PORT_CONTEXT_fpuOFFSET(8)(sp)   /* fs0 */
    fsw f9,  PORT_CONTEXT_fpuOFFSET(9)(sp)   /* fs1 */

    fsw f10, PORT_CONTEXT_fpuOFFSET(10)(sp)  /* fa0 */
    fsw f11, PORT_CONTEXT_fpuOFFSET(11)(sp)  /* fa1 */

    fsw f12, PORT_CONTEXT_fpuOFFSET(12)(sp)  /* fa2 */
    fsw f13, PORT_CONTEXT_fpuOFFSET(13)(sp)  /* fa3 */
    fsw f14, PORT_CONTEXT_fpuOFFSET(14)(sp)  /* fa4 */
    fsw f15, PORT_CONTEXT_fpuOFFSET(15)(sp)  /* fa5 */
    fsw f16, PORT_CONTEXT_fpuOFFSET(16)(sp)  /* fa6 */
    fsw f17, PORT_CONTEXT_fpuOFFSET(17)(sp)  /* fa7 */

    fsw f18, PORT_CONTEXT_fpuOFFSET(18)(sp)  /* fs2 */
    fsw f19, PORT_CONTEXT_fpuOFFSET(19)(sp)  /* fs3 */
    fsw f20, PORT_CONTEXT_fpuOFFSET(20)(sp)  /* fs4 */
    fsw f21, PORT_CONTEXT_fpuOFFSET(21)(sp)  /* fs5 */
    fsw f22, PORT_CONTEXT_fpuOFFSET(22)(sp)  /* fs6 */
    fsw f23, PORT_CONTEXT_fpuOFFSET(23)(sp)  /* fs7 */
    fsw f24, PORT_CONTEXT_fpuOFFSET(24)(sp)  /* fs8 */
    fsw f25, PORT_CONTEXT_fpuOFFSET(25)(sp)  /* fs9 */
    fsw f26, PORT_CONTEXT_fpuOFFSET(26)(sp)  /* fs10 */
    fsw f27, PORT_CONTEXT_fpuOFFSET(27)(sp)  /* fs11 */

    fsw f28, PORT_CONTEXT_fpuOFFSET(28)(sp)  /* ft8 */
    fsw f29, PORT_CONTEXT_fpuOFFSET(29)(sp)  /* ft9 */
    fsw f30, PORT_CONTEXT_fpuOFFSET(30)(sp)  /* ft10 */
    fsw f31, PORT_CONTEXT_fpuOFFSET(31)(sp)  /* ft11 */

    /* 将FS状态设置为clean */
    csrc mstatus, t0
    li t1, MSTATUS_FS_CLEAN
    csrs mstatus, t1
1:
#endif /* CONFIG_RISCV1_FPU */
.endm

/* ------------------------------------------------------------
 * 恢复浮点寄存器
 * ------------------------------------------------------------ */
.macro portRESTORE_FpuReg
#ifdef CONFIG_RISCV1_FPU
    /* Assume that a0 contain the @ of saved core register */
    /* load_x  t1, portMSTATUS_OFFSET*portWORD_SIZE(a0) */
    /* get FS field from mstatus */
    /* li t0, MSTATUS_FS */
    /* and t0, t1, t0 */
    /* li t2, MSTATUS_FS_OFF */
    /* beq t2, t0, 1f */
    /* FS == dirrty */
    /* li a0, 0xb */
    /* call tmp_trace */
    /* csrs mstatus, t0 */
    /* Remove space added for additional fpu registers. */

    /* 从栈中恢复所有浮点寄存器 */
    flw f0,  PORT_CONTEXT_fpuOFFSET(0)(sp)
    flw f1,  PORT_CONTEXT_fpuOFFSET(1)(sp)
    flw f2,  PORT_CONTEXT_fpuOFFSET(2)(sp)
    flw f3,  PORT_CONTEXT_fpuOFFSET(3)(sp)
    flw f4,  PORT_CONTEXT_fpuOFFSET(4)(sp)
    flw f5,  PORT_CONTEXT_fpuOFFSET(5)(sp)
    flw f6,  PORT_CONTEXT_fpuOFFSET(6)(sp)
    flw f7,  PORT_CONTEXT_fpuOFFSET(7)(sp)

    flw f8,  PORT_CONTEXT_fpuOFFSET(8)(sp)
    flw f9,  PORT_CONTEXT_fpuOFFSET(9)(sp)

    flw f10, PORT_CONTEXT_fpuOFFSET(10)(sp)
    flw f11, PORT_CONTEXT_fpuOFFSET(11)(sp)

    flw f12, PORT_CONTEXT_fpuOFFSET(12)(sp)
    flw f13, PORT_CONTEXT_fpuOFFSET(13)(sp)
    flw f14, PORT_CONTEXT_fpuOFFSET(14)(sp)
    flw f15, PORT_CONTEXT_fpuOFFSET(15)(sp)
    flw f16, PORT_CONTEXT_fpuOFFSET(16)(sp)
    flw f17, PORT_CONTEXT_fpuOFFSET(17)(sp)

    flw f18, PORT_CONTEXT_fpuOFFSET(18)(sp)
    flw f19, PORT_CONTEXT_fpuOFFSET(19)(sp)
    flw f20, PORT_CONTEXT_fpuOFFSET(20)(sp)
    flw f21, PORT_CONTEXT_fpuOFFSET(21)(sp)
    flw f22, PORT_CONTEXT_fpuOFFSET(22)(sp)
    flw f23, PORT_CONTEXT_fpuOFFSET(23)(sp)
    flw f24, PORT_CONTEXT_fpuOFFSET(24)(sp)
    flw f25, PORT_CONTEXT_fpuOFFSET(25)(sp)
    flw f26, PORT_CONTEXT_fpuOFFSET(26)(sp)
    flw f27, PORT_CONTEXT_fpuOFFSET(27)(sp)

    flw f28, PORT_CONTEXT_fpuOFFSET(28)(sp)
    flw f29, PORT_CONTEXT_fpuOFFSET(29)(sp)
    flw f30, PORT_CONTEXT_fpuOFFSET(30)(sp)
    flw f31, PORT_CONTEXT_fpuOFFSET(31)(sp)

    /* 恢复栈指针 */
    addi sp, sp, portasmFPU_CONTEXT_WORDSIZE
#endif /* CONFIG_RISCV1_FPU */
.endm

/* ------------------------------------------------------------
 * 保存附加寄存器（浮点+向量）
 * ------------------------------------------------------------ */
.macro portasmSAVE_ADDITIONAL_REGISTERS
    addi sp, sp, 4                    /* 为mepc预留空间 */
    portSAVE_FpuReg                   /* 保存浮点寄存器 */
    portasmSAVE_AIE_VECTOR_REGS      /* 保存向量寄存器 */
    addi sp, sp, -4                   /* 恢复栈指针 */
.endm

/* ------------------------------------------------------------
 * 恢复附加寄存器（浮点+向量）
 * ------------------------------------------------------------ */
.macro portasmRESTORE_ADDITIONAL_REGISTERS
    addi sp, sp, 4                    /* 为mepc预留空间 */
    portasmRESTORE_AIE_VECTOR_REGS   /* 恢复向量寄存器 */
    portRESTORE_FpuReg               /* 恢复浮点寄存器 */
    addi sp, sp, -4                   /* 恢复栈指针 */
.endm



/* ============================================================
 * 上下文保存与恢复主宏
 * ============================================================ */

/* ------------------------------------------------------------
 * 保存上下文（内部实现）
 * ------------------------------------------------------------ */
.macro portcontextSAVE_CONTEXT_INTERNAL
    /* 保存整数寄存器到栈 */
    addi sp, sp, -portCONTEXT_SIZE

    sw x1,  1 * portWORD_SIZE(sp)  /* ra */
    sw x5,  2 * portWORD_SIZE(sp)  /* t0 */
    sw x6,  3 * portWORD_SIZE(sp)  /* t1 */
    sw x7,  4 * portWORD_SIZE(sp)  /* t2 */
    sw x8,  5 * portWORD_SIZE(sp)  /* s0/fp */
    sw x9,  6 * portWORD_SIZE(sp)  /* s1 */
    sw x10, 7 * portWORD_SIZE(sp)  /* a0 */
    sw x11, 8 * portWORD_SIZE(sp)  /* a1 */
    sw x12, 9 * portWORD_SIZE(sp)  /* a2 */
    sw x13, 10 * portWORD_SIZE(sp) /* a3 */
    sw x14, 11 * portWORD_SIZE(sp) /* a4 */
    sw x15, 12 * portWORD_SIZE(sp) /* a5 */

#ifndef __riscv_32e
    sw x16, 13 * portWORD_SIZE(sp) /* a6 */
    sw x17, 14 * portWORD_SIZE(sp) /* a7 */
    sw x18, 15 * portWORD_SIZE(sp) /* s2 */
    sw x19, 16 * portWORD_SIZE(sp) /* s3 */
    sw x20, 17 * portWORD_SIZE(sp) /* s4 */
    sw x21, 18 * portWORD_SIZE(sp) /* s5 */
    sw x22, 19 * portWORD_SIZE(sp) /* s6 */
    sw x23, 20 * portWORD_SIZE(sp) /* s7 */
    sw x24, 21 * portWORD_SIZE(sp) /* s8 */
    sw x25, 22 * portWORD_SIZE(sp) /* s9 */
    sw x26, 23 * portWORD_SIZE(sp) /* s10 */
    sw x27, 24 * portWORD_SIZE(sp) /* s11 */
    sw x28, 25 * portWORD_SIZE(sp) /* t3 */
    sw x29, 26 * portWORD_SIZE(sp) /* t4 */
    sw x30, 27 * portWORD_SIZE(sp) /* t5 */
    sw x31, 28 * portWORD_SIZE(sp) /* t6 */
#endif

    /* 保存临界区嵌套计数 */
    lw t0, xCriticalNesting
    sw t0, portCRITICAL_NESTING_OFFSET * portWORD_SIZE(sp)

    /* 保存mstatus */
    csrr t0, mstatus
    sw t0, portMSTATUS_OFFSET * portWORD_SIZE(sp)

    /* 保存附加寄存器（浮点+向量） */
    portasmSAVE_ADDITIONAL_REGISTERS

    /* 如果是任务模式，保存栈指针到TCB */
    la t5, os_handler_mode
    lw t6, 0(t5)
    bnez t6, 2f

    lw t0, pxCurrentTCB
    bnez t0, 1f
    la t0, g_exc_stack_frame
1:
    sw sp, 0(t0)  /* 保存sp到TCB的第一个成员 */
2:
.endm

/* ------------------------------------------------------------
 * 保存异常上下文（同步异常）
 * ------------------------------------------------------------ */
.macro portcontextSAVE_EXCEPTION_CONTEXT
    portcontextSAVE_CONTEXT_INTERNAL

    /* 获取异常原因和PC */
    csrr a0, mcause
    csrr a1, mepc
    addi a1, a1, 4  /* 同步异常：PC指向下一条指令 */
    sw a1, 0(sp)  /* 保存更新后的PC */

    /* 切换到ISR栈 */
    la t0, os_handler_mode
    lw t1, 0(t0)
    bnez t1, 1f
    la sp, _exception_stack_end
1:
    addi t1, t1, 1
    sw t1, 0(t0)
.endm

/* ------------------------------------------------------------
 * 保存中断上下文（异步中断）
 * ------------------------------------------------------------ */
.macro portcontextSAVE_INTERRUPT_CONTEXT
    portcontextSAVE_CONTEXT_INTERNAL

    /* 获取中断原因和PC */
    csrr a0, mcause
    csrr a1, mepc
    sw a1, 0(sp)  /* 异步中断：保存原始PC */

    /* 切换到ISR栈 */
    la t0, os_handler_mode
    lw t1, 0(t0)
    bnez t1, 1f
    la sp, _irq_stack_end
1:
    addi t1, t1, 1
    sw t1, 0(t0)
.endm

/* ------------------------------------------------------------
 * 恢复上下文（用于异常/中断返回）
 * ------------------------------------------------------------ */
.macro portcontextRESTORE_CONTEXT
    /* 减少处理程序模式计数 */
    la t0, os_handler_mode
    lw t1, 0(t0)
    addi t1, t1, -1
    sw t1, 0(t0)
    bnez t1, 2f  /* 如果还在处理程序模式，跳过TCB切换 */

    /* 从TCB恢复栈指针 */
    lw t1, pxCurrentTCB
    bnez t1, 1f
    la t1, g_exc_stack_frame
1:
    lw sp, 0(t1)
2:

    /* 恢复mepc */
    lw t0, 0(sp)
    csrw mepc, t0

    /* 恢复附加寄存器 */
    portasmRESTORE_ADDITIONAL_REGISTERS

    /* 恢复mstatus */
    lw t0, portMSTATUS_OFFSET * portWORD_SIZE(sp)
    csrw mstatus, t0

    /* 恢复临界区嵌套计数 */
    lw t0, portCRITICAL_NESTING_OFFSET * portWORD_SIZE(sp)
    lw t1, pxCriticalNesting
    sw t0, 0(t1)

    /* 恢复整数寄存器 */
    lw x1,  1 * portWORD_SIZE(sp)  /* ra */
    lw x5,  2 * portWORD_SIZE(sp)  /* t0 */
    lw x6,  3 * portWORD_SIZE(sp)  /* t1 */
    lw x7,  4 * portWORD_SIZE(sp)  /* t2 */
    lw x8,  5 * portWORD_SIZE(sp)  /* s0/fp */
    lw x9,  6 * portWORD_SIZE(sp)  /* s1 */
    lw x10, 7 * portWORD_SIZE(sp)  /* a0 */
    lw x11, 8 * portWORD_SIZE(sp)  /* a1 */
    lw x12, 9 * portWORD_SIZE(sp)  /* a2 */
    lw x13, 10 * portWORD_SIZE(sp) /* a3 */
    lw x14, 11 * portWORD_SIZE(sp) /* a4 */
    lw x15, 12 * portWORD_SIZE(sp) /* a5 */

#ifndef __riscv_32e
    lw x16, 13 * portWORD_SIZE(sp) /* a6 */
    lw x17, 14 * portWORD_SIZE(sp) /* a7 */
    lw x18, 15 * portWORD_SIZE(sp) /* s2 */
    lw x19, 16 * portWORD_SIZE(sp) /* s3 */
    lw x20, 17 * portWORD_SIZE(sp) /* s4 */
    lw x21, 18 * portWORD_SIZE(sp) /* s5 */
    lw x22, 19 * portWORD_SIZE(sp) /* s6 */
    lw x23, 20 * portWORD_SIZE(sp) /* s7 */
    lw x24, 21 * portWORD_SIZE(sp) /* s8 */
    lw x25, 22 * portWORD_SIZE(sp) /* s9 */
    lw x26, 23 * portWORD_SIZE(sp) /* s10 */
    lw x27, 24 * portWORD_SIZE(sp) /* s11 */
    lw x28, 25 * portWORD_SIZE(sp) /* t3 */
    lw x29, 26 * portWORD_SIZE(sp) /* t4 */
    lw x30, 27 * portWORD_SIZE(sp) /* t5 */
    lw x31, 28 * portWORD_SIZE(sp) /* t6 */
#endif

    /* 恢复栈指针并返回 */
    addi sp, sp, portCONTEXT_SIZE
    mret
.endm

#endif /* __ASSEMBLY__ */
