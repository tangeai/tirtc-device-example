#include "soc/riscv.h"
#include <driver/irq.h>
#include <stdio.h>
#include "io.h"
#include "assert.h"
#include <cpu/irq.h>
#include "riscv_cpu.h"
#include "soc/plic.h"
#include <common.h>
#include "spinlock.h"

#ifdef CONFIG_TCSM_SECTION_IRQ
#include <cpu/tcsm_section.h>
#else
#include <tcsm_section_null.h>
#endif

// #define DEBUG_IRQ
#ifdef DEBUG_IRQ
#define DEBUG_IRQ_p(fmt, args...) printf("[IRQ]-[%s]-[%d] "fmt, __func__, __LINE__, ##args)
#else
#define DEBUG_IRQ_p(fmt, args...)
#endif

struct irq_flag {
    unsigned int enabled : 1;
    unsigned int requested : 1;
    unsigned int irq_force_disable : 1;
};

struct irq_data {
    void *data;
    const char *name;
    irq_handler_t handler;
};

struct irq_flag irqflag[IRQ_NUMS];
struct irq_data irqdata[IRQ_NUMS];

static DEFINE_SPINLOCK(lock);

/*
 * Reserved interrupt ISR.
 */
static void do_reservedIRQ(int irq, void *data)
{
    (void) data;
    panic("** Interrupt %d (%p %s) - miss ISR **\n", irq, data, irqdata[irq].name);
}

void irq_dispatcher_init(void)
{
    int index;

    for (index = 0; index < IRQ_NUMS; index++)
        irqdata[index].handler = do_reservedIRQ;
}

void arch_handle_irq(int irq)
{
    DEBUG_IRQ_p(" - %s - irq: %d\n", __func__, irq);
    irqdata[irq].handler(irq, irqdata[irq].data);
}

void arch_request_irq_disabled(int irq, unsigned int irq_flags, irq_handler_t handler, const char *name, void *data)
{
    unsigned long flags;

    assert(handler);
    assert_range(irq, 0, IRQ_NUMS - 1);

    spin_lock_irqsave(&lock, flags);

    DEBUG_IRQ_p("- %s - name: %s, irq: %d\n", __func__, name, irq);
    assert(irqdata[irq].handler == do_reservedIRQ);

    if (!irqflag[irq].requested) {
        irqdata[irq].handler = handler;
        irqdata[irq].data = data;
        irqdata[irq].name = name;

        if (irq < IRQ_CPU_END)
            disable_vect_int_irq(irq);
        else if (irq < IRQ_INTC_END)
            disable_intc_irq(irq);
        else if (irq < IRQ_GPIO_END)
            soc_gpio_startup_irq(irq, irq_flags);
        else
            soc_startup_extra_irq(irq, irq_flags);

        irqflag[irq].requested = 1;
    }

    spin_unlock_irqrestore(&lock, flags);
}

void arch_request_irq(int irq, unsigned int irq_flags, irq_handler_t handler, const char *name, void *data)
{
    DEBUG_IRQ_p("- %s -  name: %s - irq : %d\n", __func__, name, irq);
    request_irq_disabled(irq, irq_flags, handler, name, data);
    enable_irq(irq);
}

void arch_release_irq(int irq)
{
    unsigned long flags;

    assert_range(irq, 0, IRQ_NUMS - 1);

    spin_lock_irqsave(&lock, flags);

    assert(irqdata[irq].handler);
    assert(!irqflag[irq].enabled);

    if (irqflag[irq].requested) {
        irqdata[irq].handler = do_reservedIRQ;
        irqdata[irq].data = NULL;
        irqdata[irq].name = NULL;

        if (irq < IRQ_CPU_END)
            disable_vect_int_irq(irq);
        else if (irq < IRQ_INTC_END)
            disable_intc_irq(irq);
        else if (irq < IRQ_GPIO_END)
            soc_gpio_shutdown_irq(irq);
        else
            soc_shutdown_extra_irq(irq);

        irqflag[irq].requested = 0;
    }

    spin_unlock_irqrestore(&lock, flags);
}

void arch_release_all_irq(void)
{
    int irq;
    unsigned long flags;

    spin_lock_irqsave(&lock, flags);

    for (irq = 0; irq < IRQ_NUMS; irq++) {
        if (!irqflag[irq].requested)
            continue;

        irqdata[irq].handler = do_reservedIRQ;
        irqdata[irq].data = NULL;
        irqdata[irq].name = NULL;

        if (irq < IRQ_CPU_END)
            disable_vect_int_irq(irq);
        else if (irq < IRQ_INTC_END)
            disable_intc_irq(irq);
        else if (irq < IRQ_GPIO_END) {
            soc_gpio_disable_irq(irq);
            soc_gpio_shutdown_irq(irq);
        } else {
            soc_disable_extra_irq(irq);
            soc_shutdown_extra_irq(irq);
        }

        irqflag[irq].enabled = 0;
        irqflag[irq].requested = 0;
    }

    spin_unlock_irqrestore(&lock, flags);
}

void arch_enable_irq_nolock(int irq)
{
    assert_range(irq, 0, IRQ_NUMS - 1);

    assert(irqdata[irq].handler);

    if (!irqflag[irq].enabled) {
        if (irq < IRQ_CPU_END)
            enable_vect_int_irq(irq);
        else if (irq < IRQ_INTC_END)
            enable_intc_irq(irq);
        else if (irq < IRQ_GPIO_END)
            soc_gpio_enable_irq(irq);
        else
            soc_enable_extra_irq(irq);

        irqflag[irq].enabled = 1;
    }
}

void arch_disable_irq_nolock(int irq)
{
    assert_range(irq, 0, IRQ_NUMS - 1);

    assert(irqdata[irq].handler);

    if (irqflag[irq].enabled) {
        if (irq < IRQ_CPU_END)
            disable_vect_int_irq(irq);
        else if (irq < IRQ_INTC_END)
            disable_intc_irq(irq);
        else if (irq < IRQ_GPIO_END)
            soc_gpio_disable_irq(irq);
        else
            soc_disable_extra_irq(irq);

        irqflag[irq].enabled = 0;
    }
}

void arch_enable_irq(int irq)
{
    unsigned long flags;

    spin_lock_irqsave(&lock, flags);

    arch_enable_irq_nolock(irq);

    spin_unlock_irqrestore(&lock, flags);
}

void arch_disable_irq(int irq)
{
    unsigned long flags;

    spin_lock_irqsave(&lock, flags);

    arch_disable_irq_nolock(irq);

    spin_unlock_irqrestore(&lock, flags);
}

__weak void soc_enable_extra_irq(int irq)
{
    (void) irq;
    assert(0);
}

__weak void soc_disable_extra_irq(int irq)
{
    (void) irq;
    assert(0);
}

__weak void soc_startup_extra_irq(int irq, unsigned int irq_flags)
{
    (void) irq;
    (void) irq_flags;
    assert(0);
}

__weak void soc_shutdown_extra_irq(int irq)
{
    (void) irq;
    assert(0);
}

__weak int soc_gpio_to_irq(int gpio)
{
    return gpio >= 0 ? IRQ_GPIO_START + gpio : -1;
}

__weak int soc_irq_to_gpio(int irq)
{
    return irq - IRQ_GPIO_START;
}
