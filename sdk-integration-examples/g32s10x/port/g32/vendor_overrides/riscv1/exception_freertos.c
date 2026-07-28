#include <core.h>
#include <stdint.h>
#include <FreeRTOS.h>
#include <task.h>
#include <driver/irq.h>
#include <stdio.h>
#include <common.h>
#include "lds_symbol.h"
#include "encoding.h"
#include "exception.h"

void machine_software_interrupt_handler (uint32_t ulMcause);
void machine_external_interrupt_handler (void);
void freertos_risc_v_application_exception_handler(uint32_t ulMcause);

void freertos_risc_v_application_interrupt_handler(uint32_t ulMcause)
{
#ifdef config_DEBUG_ISR_LAYTENCY
	uint64_t interrupt_time = soc_systick_get_time_ns();
#endif

	switch(ulMcause) {

        case (MCAUSE_MACHINE_SOFTWARE_INTERRUP):      machine_software_interrupt_handler(ulMcause); break;
        case (MCAUSE_MACHINE_TIMER_INTERRUP):         // in vectord mode, MACHINE_TIMER_INTERRUP should be directed to mtvec vectortable
            printf("Error: Unexpected machine-mode interrupt !  Terminating ...\n");
            freertos_risc_v_application_exception_handler(ulMcause);
            break;
        case (MCAUSE_MACHINE_EXTARNAL_INTERRUP):      machine_external_interrupt_handler(); break;

        case (MCAUSE_SUPERVISOR_SOFTWARE_INTERRUP):
        case (MCAUSE_SUPERVISOR_TIMER_INTERRUP):
        case (MCAUSE_SUPERVISOR_EXTERNAL_INTERRUP):
            printf("Error: NOT-implemented, Supervisor-Mode interrupt entered! Terminating ...\n");
            freertos_risc_v_application_exception_handler(ulMcause);
            break;

        case (MCAUSE_USER_SOFTWARE_INTERRUP):
        case (MCAUSE_USER_TIMER_INTERRUP):
        case (MCAUSE_USER_EXTERNAL_INTERRUP):{
            printf("Error: NOT-implemented,User-Mode interrupt entered! Terminating ...\n");
            freertos_risc_v_application_exception_handler(ulMcause);}break;

        default:{
            uint32_t mip = read_csr(mip);
            uint32_t mie = read_csr(mie);
            printf("Error: interrupt by unknown local interrupt! mip:0x%x mie:0x%x mip&mie=0x%x Terminating ...\n", (unsigned int)mip, (unsigned int)mie, (unsigned int)(mip&mie));
            freertos_risc_v_application_exception_handler(ulMcause);
        }break;
	}

    #ifdef config_DEBUG_ISR_LAYTENCY
    uint64_t isr_laytency=soc_systick_get_time_ns() - interrupt_time;
    printf("irq: isr_laytency:%d \n", isr_laytency);
    #endif
}

volatile int dump_stack_top = 0;
extern uint32_t *pxCurrentTCB;

static void dump_register(uint32_t *addr)
{
	printf("EPC:0x%08lx ra:0x%08lx t0:0x%08lx t1:0x%08lx t2:0x%08lx s0:0x%08lx s1:0x%08lx\n",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6]);
	printf("a0:0x%08lx a1:0x%08lx a2:0x%08lx a3:0x%08lx a4:0x%08lx a5:0x%08lx a6:0x%08lx\n",
			addr[7], addr[8], addr[9], addr[10], addr[11], addr[12], addr[13]);
	printf("a7:0x%08lx s2:0x%08lx s3:0x%08lx s4:0x%08lx s5:0x%08lx s6:0x%08lx s7:0x%08lx\n",
			addr[14], addr[15], addr[16], addr[17], addr[18], addr[19], addr[20]);
	printf("s8:0x%08lx s9:0x%08lx s10:0x%08lx s11:0x%08lx t3:0x%08lx t4:0x%08lx t5:0x%08lx\n",
			addr[21], addr[22], addr[23], addr[24], addr[25], addr[26], addr[27]);
	printf("t6:0x%08lx CRN:0x%08lx MST:0x%08lx \n",
			addr[28], addr[29], addr[30]);
}
static void dump_addr_data(uint32_t *addr, int szWords)
{
	for (int i = 0; i < szWords; i++) {
		if ((i % 8) == 0) {
			printf("%p: ", addr + i);
		}
		printf(" 0x%08lx", *(addr + i));
		if ((i % 8) == 7) {
			printf("\n");
		}
	}
}

// void *pcTaskStackStart(TaskHandle_t xTaskToQuery);
void dump_stack_addr(void)
{
	uint32_t *tcb = (uint32_t *)pxCurrentTCB;
    uint32_t *sp;

    if (tcb == NULL) {
        printf("Task Name: NULL, exception happened before first task start!\n");
        return;
    }

    sp = (uint32_t *)(uintptr_t)tcb[0];
    printf("Task Name: %s\n", pcTaskGetName((TaskHandle_t)pxCurrentTCB));
	// printk("Task start: %p\n", pcTaskStackStart((TaskHandle_t)pxCurrentTCB));
	printf("------------- register dump sp:[%p]---------------\n", sp);
	dump_register(sp);
	printf("------------- stack dump ---------------\n");
	dump_addr_data(sp - 32, 32);
	printf("------------- --------- ---------------\n");
	dump_addr_data(sp + 32, 32);
    printf("------------- --------- ---------------\n");
	dump_addr_data(sp + 64, 32);
    printf("------------- --------- ---------------\n");
	dump_addr_data(sp + 96, 32);
    printf("------------- --------- ---------------\n");
	dump_addr_data(sp + 128, 32);
    printf("------------- --------- ---------------\n");
	dump_addr_data(sp + 160, 32);
    printf("------------- --------- ---------------\n");
    dump_addr_data(sp + 192, 32);
    printf("------------- --------- ---------------\n");
	dump_addr_data(sp + 224, 32);
}

void xTaskIncrementTick_call(void)
{
	if (dump_stack_top) {
		dump_stack_addr();
	}
}


/* m-mode interrupts*/
void machine_software_interrupt_handler (uint32_t ulMcause)
{
	/*not implemented*/
    printf("machine_software_interrupt_handler NOT-implemented, returning now...\n");
}


/* exception handlers*/
/*fix misaligned address*/
bool address_misaligned_exception_handler(void *excContext)
{
    /* TODO: code that can fix load store address_misaligned_exception can be added here,
    make sure that this function return ture if this exception is could be handled by 'lb' instruvtion successfully*/

    /* Not implemented.*/
    printf("address_misaligned_exception fix NOT-implemented\n");
    return false;

}

void illegal_instruction_exc_handler(uint32_t ulMEPC)
{
    uint32_t addr=ulMEPC-4*8;
    printf("instruction near the MEPC: \n");
    if(IS_IN_TEXT_SEGMENT(ulMEPC)){
        printf("address: value \n");
        for(int i=0; i<16;i++){
            if( IS_WORD_ALIGNED(addr) ) {
                printf("0x-8%x 0x-8%x\n", addr, *(uint32_t *)addr);
                addr += 4;
            } else if( IS_HALFWORD_ALIGNED(addr) ){
                printf("0x-8%x 0x-8%x\n", addr, *(uint16_t *)addr);
                addr += 2;
            }else {
                printf("0x-8%x 0x-8%x\n", addr, *(uint8_t *)addr);
                addr += 1;
            }
        }

    }
}


void freertos_risc_v_application_exception_handler(uint32_t ulMcause)
{
	volatile uint32_t ulMEPC = 0UL, ulMCAUSE = 0UL;
	__asm volatile("csrr %0, mepc" : "=r"(ulMEPC));
	__asm volatile("csrr %0, mcause" : "=r"(ulMCAUSE));

	switch(ulMCAUSE) {
        case (MCAUSE_LOAD_ADDRSS_MISALIGNED):
        case (MCAUSE_STORE_OR_AMO_ADDRESS_MISALIGNED):{
            if(address_misaligned_exception_handler(NULL) == false){
                return;
            }

        }break;

        case (MCAUSE_INSTRUCTION_ADDRESS_MISALIGNED):
        case (MCAUSE_INSTRUCTION_ACCESS_FAULT):
        case (MCAUSE_ILLEGAL_INSTRUCTION):illegal_instruction_exc_handler(ulMEPC);break;
        case (MCAUSE_BREAKPOINT):
        case (MCAUSE_LOAD_ACCESS_FAULT):
        case (MCAUSE_STORE_OR_AMO_ACCESS_FAULT):
        case (MCAUSE_ENVIRONMENT_CALL_FROM_U_MODE):
        case (MCAUSE_ENVIRONMENT_CALL_FROM_S_MODE):
        case (MCAUSE_ENVIRONMENT_CALL_FROM_M_MODE):
        case (MCAUSE_INSTRUCTION_PAGE_FAULT):
        case (MCAUSE_LOAD_PAGE_FAULT):
        case (MCAUSE_STORE_OR_AMO_PAGE_FAULT):
        default:
        break;
    }
	/* un-resolvable interrupt/exception reached, disable global interrupt*/
	clear_csr(mstatus, MSTATUS_MIE);
	/* Store a few register values that might be useful when determining why this
	 * function was called.
	 * */


	printf("---- exception hanppend, ulMEPC: %lx, ulMcause: %lx ", ulMEPC, ulMCAUSE);

	exception_print_mcause(ulMcause);

    unsigned int *p_exc_stac_frame = 0;
    if (pxCurrentTCB) {
        p_exc_stac_frame = (unsigned int *)(pxCurrentTCB[0]);
    } else {
        p_exc_stac_frame = (unsigned int *)g_exc_stack_frame;
    }
    exception_show_exc_stack_frame_registers(p_exc_stac_frame);
#ifdef CONFIG_DUMP_STACK
    unsigned int *stack_frame = exception_get_stack_frame(p_exc_stac_frame);
    show_stacktrace((unsigned long)stack_frame, p_exc_stac_frame[0],
                    read_csr(mepc));
#endif
	dump_stack_addr();
	asm volatile("nop");
	while (1);
}
