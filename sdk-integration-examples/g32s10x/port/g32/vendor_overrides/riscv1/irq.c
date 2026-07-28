#include <driver/cache.h>
#include <driver/irq.h>
#include <os.h>
#include <riscv.h>
#include "common.h"
#include "lds_symbol.h"
#include "soc/plic.h"


void (*exception_handlers[32])(void);

void enable_vect_int_irq(int irq)
{
    set_csr(mie, 1 << irq);
}

void disable_vect_int_irq(int irq)
{
    uint32_t value = 0;
    value = read_csr(mie);
    value &= ~(1 << irq);
    set_csr(mie, value);
}

void save_and_disable_other_vect_int_irqs(unsigned int not_disable_irqs)
{
    panic("why here is save_and_disable_other_vect_int_irqs!\n");
}

void restore_save_vect_int_irqs(void)
{
    panic("why here is restore_save_vect_int_irqs!\n");
}

void set_exception_handler(unsigned int i, void (*handler)(void))
{
    assert(i < 32);
    exception_handlers[i] = handler;
}

#ifndef CONFIG_TO_BE_CHANGE
struct pt_regs {
    unsigned long regs[32];
    unsigned long lo;
    unsigned long hi;
    unsigned long epc;
    unsigned long status;
    unsigned long badva;
    unsigned long cause;
};

static const char *exception_names[32] = {
    [0] = "Instruction address misaligned",
    [1] = "Instruction access fault",
    [2] = "Illegal instruction",
    [3] = "Breakpoint",
    [4] = "Load address misaligned",
    [5] = "Load access fault",
    [6] = "Store/AMO address misaligned",
    [7] = "Store/AMO access fault",
    [8] = "Environment call from U-mode",
    [9] = "Environment call from S-mode",
    [10] = "Reserved",
    [11] = "Environment call from M-mode",
    [12] = "Instruction page fault",
    [13] = "Load page fault",
    [14] = "reserved",
    [15] = "Store/AMO page fault",
    [31] = "Unknown cause",
};

#include <cpu/cpu.h>

/*
 * 被 handle_exceptions_default 调用
 */
void do_exception_default(struct pt_regs *regs)
{
    unsigned int mcause = csr_read(MCAUSE);
    unsigned int code = get_bit_field(mcause, 0, 30);
    if (code > 15)
        code = 31;

    os_enter_critical();

    printf("This is %s expection. code: %d cpu%d\n", exception_names[code], code, read_csr(mhartid));
    printf("epc: %08lx badva: %08lx\n", regs->epc, regs->badva);

    unsigned long *r = regs->regs;
    printf("zero at v0 v1: %08lx, %08lx, %08lx, %08lx\n", 0l, r[1], r[2], r[3]);
    printf("  a0 a1 a2 a3: %08lx, %08lx, %08lx, %08lx\n", r[4], r[5], r[6], r[7]);
    printf("  t0 t1 t2 t3: %08lx, %08lx, %08lx, %08lx\n", r[8], r[9], r[10], r[11]);
    printf("  t4 t5 t6 t7: %08lx, %08lx, %08lx, %08lx\n", r[12], r[13], r[14], r[15]);
    printf("  s0 s1 s2 s3: %08lx, %08lx, %08lx, %08lx\n", r[16], r[17], r[18], r[19]);
    printf("  s4 s5 s6 s7: %08lx, %08lx, %08lx, %08lx\n", r[20], r[21], r[22], r[23]);
    printf("  t8 t9 k0 k1: %08lx, %08lx, %08lx, %08lx\n", r[24], r[25], r[26], r[27]);
    printf("  gp sp s8 ra: %08lx, %08lx, %08lx, %08lx\n", r[28], r[29], r[30], r[31]);

    printf("cause: %08lx status: %08lx\n", regs->cause, regs->status);
    printf("lo: %08lx hi: %08lx\n", regs->lo, regs->hi);

#ifdef CONFIG_DUMP_STACK
    show_stacktrace(regs->regs[29], regs->epc, regs->regs[31]);
#endif

    /*
     * Game over - no way to handle this if it ever occurs. Most probably
     * caused by a new unknown cpu type or after another deadly
     * hard/software error.
     */
    printf("Game over\n");

    os_exit_critical();

    while(1);
}
#endif
/*-----------------------------------------------------------*/

void arch_irq_init(void)
{
    clear_csr(mie, MIP_MEIP);
    clear_csr(mip, MIP_MTIP);

    irq_dispatcher_init();

    soc_irq_init();
}
