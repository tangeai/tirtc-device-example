
#include <stdio.h>
#include <stdint.h>
#include "riscv_cpu.h"
#include "exception.h"
#include "encoding.h"
#include "core.h"

uint32_t g_exc_stack_frame;

const char *mcause_messages[32] = {
    [0]="Instruction address misaligned",
    [1]="Instruction access fault",
    [2]="Illegal instruction",
    [3]="Breakpoint",
    [4]="Load address misaligned",
    [5]="Load access fault",
    [6]="Store/AMO address misaligned",
    [7]="Store/AMO access fault",
    [8]="Environment call from U-mode",
    [9]="Environment call from S-mode",
    [10]="Reserved",
    [11]="Environment call from M-mode",
    [12]="Instruction page fault",
    [13]="Load page fault",
    [14]="reserved",
    [15]="Store/AMO page fault",
    [31]="Unknown cause",
};


void exception_print_mcause(uint32_t mcause)
{
    if (mcause >= 0 && mcause < 15) {
        printf("%s\n", mcause_messages[mcause]);
    } else {
        printf("%s\n", mcause_messages[31]); // Unknown cause
    }
}

unsigned int *exception_get_stack_frame(unsigned int *sf)
{
    return sf + portasmADDITIONAL_CONTEXT_SIZE / 4 + portCONTEXT_SIZE / 4;
}

VCSR_Register_t vcsr;
void exception_show_exc_stack_frame_registers(unsigned int *sf)
{
    // Print the exception-related registers
    printf("Exception occurred! Exception register info:\n");
    printf("mcause: 0x%08x   ", read_csr(mcause));
    printf("mepc: 0x%08x\n", read_csr(mepc));
    printf("mtval: 0x%08x\n", read_csr(mtval));
    printf("mstatus: 0x%08x\n", read_csr(mstatus));
    printf("mtvec: 0x%08x\n", read_csr(mtvec));
    printf("mip: 0x%08x\n", read_csr(mip));
    printf("mie: 0x%08x\n\n", read_csr(mie));
    //printf("tp: 0x%08x\n", stack_frame[IDX_X4_TP]);
    //printf("gp: 0x%08x\n", stack_frame[IDX_X3_GP]);

    // Print the register stack frame
    //printf("pvParameters: 0x%08x\n", stack_frame[IDX_PVPARAMETERS]);
    register void *gp_value asm("gp");
    register void *tp_value asm("tp");

    unsigned int *stack_frame = exception_get_stack_frame(sf);
    printf("Exception StackFrame Info:\n");
    printf("sp: 0x%08x\n", stack_frame);
    printf("gp: 0x%08x\n", gp_value);
    printf("tp: 0x%08x\n", tp_value);
    printf("xCriticalNesting: 0x%08x\n", stack_frame[IDX_XCRITICALNESTING]);
    printf("mstatus: 0x%08x\n", stack_frame[IDX_MSTATUS]);
    printf("mepc: 0x%08x\n", sf[0]);
        // Print ra
    printf("ra: 0x%08x\n\n", stack_frame[IDX_X1_RA]);
    // Print t0-t6
    printf("t0: 0x%08x\n", stack_frame[IDX_X5_T0]);
    printf("t1: 0x%08x\n", stack_frame[IDX_X6_T1]);
    printf("t2: 0x%08x\n", stack_frame[IDX_X7_T2]);
    printf("t3: 0x%08x\n", stack_frame[IDX_X28_T3]);
    printf("t4: 0x%08x\n", stack_frame[IDX_X29_T4]);
    printf("t5: 0x%08x\n", stack_frame[IDX_X30_T5]);
    printf("t6: 0x%08x\n\n", stack_frame[IDX_X31_T6]);

    // Print a0-a7
    printf("a0: 0x%08x\n", stack_frame[IDX_X10_A0]);
    printf("a1: 0x%08x\n", stack_frame[IDX_X11_A1]);
    printf("a2: 0x%08x\n", stack_frame[IDX_X12_A2]);
    printf("a3: 0x%08x\n", stack_frame[IDX_X13_A3]);
    printf("a4: 0x%08x\n", stack_frame[IDX_X14_A4]);
    printf("a5: 0x%08x\n", stack_frame[IDX_X15_A5]);
    #ifndef __riscv_32e
        printf("a6: 0x%08x\n", stack_frame[IDX_X16_A6]);
        printf("a7: 0x%08x\n\n", stack_frame[IDX_X17_A7]);
    #endif /* ifndef __riscv_32e */

    // Print s0-s11
    printf("s0/fp: 0x%08x\n", stack_frame[IDX_X8_S0_FP]);
    printf("s1: 0x%08x\n", stack_frame[IDX_X9_S1]);
    #ifndef __riscv_32e
        printf("s2: 0x%08x\n", stack_frame[IDX_X18_S2]);
        printf("s3: 0x%08x\n", stack_frame[IDX_X19_S3]);
        printf("s4: 0x%08x\n", stack_frame[IDX_X20_S4]);
        printf("s5: 0x%08x\n", stack_frame[IDX_X21_S5]);
        printf("s6: 0x%08x\n", stack_frame[IDX_X22_S6]);
        printf("s7: 0x%08x\n", stack_frame[IDX_X23_S7]);
        printf("s8: 0x%08x\n", stack_frame[IDX_X24_S8]);
        printf("s9: 0x%08x\n", stack_frame[IDX_X25_S9]);
        printf("s10: 0x%08x\n", stack_frame[IDX_X26_S10]);
        printf("s11: 0x%08x\n", stack_frame[IDX_X27_S11]);
    #endif /* ifndef __riscv_32e */


    #ifdef CONFIG_RISCV1_FPU
        #ifdef CONFIG_OS

        uint32_t *p_stack_frame_fpu =
            (uint32_t *)(void *)(sf + portasmVXU_CONTEXT_WORDSIZE / 4 + 1);
        #else
        // TODO:
        uint32_t *p_stack_frame_fpu = NULL;
        printf(" TODO:FPU exception stackframe not implemented \n");
        #endif
        uint32_t fcsr = read_csr(fcsr);
        fcsr_t *pfcsr = (fcsr_t *)&fcsr;
        riscv_cpu_reg_info_fcsr(fcsr);
        if (pfcsr->fflags) {
            riscv_cpu_reg_info_floating_point(p_stack_frame_fpu);
        }
    #endif /* CONFIG_RISCV1_FPU */

    #ifdef CONFIG_RISCV1_AIE_VECTOR
        uint32_t *p_stack_frame_vector = (uint32_t *)(void *)(sf + 1);

        vcsr._w = arch_get_vcsr();

        arch_vcsr_info(vcsr);

        if (((vcsr._b.Xcause & vcsr._b.Xenables) != 0) || (vcsr._b.Xflags != 0)) {
            arch_dump_ingenic_aie_vector_registers(p_stack_frame_vector);
        }
    #endif
}
