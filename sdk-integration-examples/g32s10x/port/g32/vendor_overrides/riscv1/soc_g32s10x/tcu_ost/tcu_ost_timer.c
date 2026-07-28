#include <common.h>
#include <driver/clk.h>
#include <soc/base.h>
#include <soc/extal.h>
// #include <driver/irq.h>
#include <driver/systick.h>

#include <driver/irq.h>
#include "tcu_ost_timer_regs.h"

#define EXTAL_CLOCK_MHZ         (JZ_EXTAL / USEC_PER_SEC)
#define OST_TIMER_MAX_VALUE         0xFFFFFFFFULL
#define OST_TIMER_MAX_DELTA_US      (OST_TIMER_MAX_VALUE / EXTAL_CLOCK_MHZ)

union cycle_type {
    uint64_t cycle64;
    uint32_t cycle32[2];
};

#define TCU_OST_PRECALE                  1

#define TCU_TESR    (0x14)    /*Timer Counter Enable Set Register */
#define TCU_TECR    (0x18)    /*Timer Counter Enable Clear Register */
#define TCU_TMSR    (0x34)    /*Timer Counter Mask Set Register */
#define TCU_TMCR    (0x38)    /*Timer Counter Mask Clear Register */
#define TCU_TFSTR   (0x24)    /*Timer Counter Flag Set Register */
#define TCU_TFCLR   (0x28)    /*Timer Counter Flag Clear Register */


/* TCU TER */
#define TER_OSTEN       15
#define TER_OSTCL       15

/* TCU TMR */
#define TMR_OSTMCL     15
#define TMR_OSTMST     15

/* TCU TFR */
#define TFR_OSTFCL     15
#define TFR_OSTFST     15


/*OST*/
#define TCU_OSTFULL        (0x100)
#define TCU_OSTCNTL        (0x104)
#define TCU_OSTCNTH        (0x108)
#define TCU_OSTCSR         (0x110)

/* OST TCSR */
#define OSTCSR_CNT_MD      (1 << 22)
#define OSTCSR_SD          (1 << 15)

#define OSTCSR_EXT_EN      (1 << 2)

#define TCU_ADDR(reg)    (io_addr(TCU_IOBASE + reg))

static struct clk *tcu_clk;

static void m_default_handler(void) {}

static systick_event_handler_t clint_callback_handler = m_default_handler;


static inline uint32_t tcu_read_reg(int reg)
{
    return *TCU_ADDR(reg);
}

static inline void tcu_write_reg(int reg, uint32_t value)
{
    *TCU_ADDR(reg) = value;
}

static inline void tcu_set_bit(int reg, int n)
{
    set_bits(*TCU_ADDR(reg), BIT(n));
}

static inline void tcu_clear_bit(int reg, int n)
{
    clear_bits(*TCU_ADDR(reg), BIT(n));
}

static inline int tcu_prescale(int prescale)
{
    if (prescale == 1) return 0;
    if (prescale == 4) return 1 << 3;
    if (prescale == 16) return 1 << 4;
    return 2;
}

static inline union cycle_type tcu_ost_get_cycle(void)
{
    union cycle_type cycle;

    do {
        cycle.cycle32[1] = tcu_read_reg(TCU_OSTCNTH);
        cycle.cycle32[0] = tcu_read_reg(TCU_OSTCNTL);
    } while(cycle.cycle32[1] != tcu_read_reg(TCU_OSTCNTH));

    return cycle;
}

static inline void tcu_ost_start(void)
{
    /* enable systick interrupt */
    tcu_set_bit(TCU_TMCR, TMR_OSTMCL);
    tcu_set_bit(TCU_TESR, TER_OSTEN);
    tcu_write_reg(TCU_OSTCNTL, 0);
    tcu_write_reg(TCU_OSTCNTH, 0);
}

static inline void tcu_ost_stop(void)
{
    /* mask systick interrupt */
    tcu_set_bit(TCU_TFCLR, TFR_OSTFCL);
    /* disable systick interrupt */
    tcu_set_bit(TCU_TMSR, TMR_OSTMST);
    tcu_set_bit(TCU_TECR, TER_OSTCL);
}


static void tcu_irq_handler(int irq, void *data)
{
    /* disable systick counter */
    tcu_ost_stop();
    clint_callback_handler();
}


static void tcu_ost_init(void)
{
#ifndef CONFIG_RUN_ON_FPGA
    /* enable clk */
    tcu_clk = clk_get("gate_ost");
    clk_enable(tcu_clk);
#endif

    tcu_write_reg(TCU_OSTCNTL, 0);
    tcu_write_reg(TCU_OSTCNTH, 0);

    tcu_write_reg(TCU_OSTCSR,(tcu_prescale(TCU_OST_PRECALE) | OSTCSR_EXT_EN));
    request_irq(IRQ_TCU_OST, 0, tcu_irq_handler, "tcu", NULL);
    set_irq_priority(IRQ_TCU_OST, IRQ_PRIORITY_MIDDLE);

    /* enable systick counter */
    tcu_set_bit(TCU_TESR, TER_OSTEN);
}

static inline uint64_t clint_get_cycle(void)
{
    union cycle_type cycle = {0};

    do {
        cycle.cycle32[1] = clint_read_reg(MTIMEH);
        cycle.cycle32[0] = clint_read_reg(MTIMEL);
    } while(cycle.cycle32[1] != clint_read_reg(MTIMEH));

    return cycle.cycle64;
}

static int is_inited = 0;

uint64_t soc_systick_get_time_us(void)
{
    if (!is_inited)
        return 0;

    return clint_get_cycle() / EXTAL_CLOCK_MHZ;
}

uint64_t soc_systick_get_time_ns(void)
{
    if (!is_inited)
        return 0;

    return clint_get_cycle() * 1000 / EXTAL_CLOCK_MHZ;
}

void ndelay(unsigned int nsecs)
{
    uint64_t start = clint_get_cycle();
    uint64_t cycles = (uint64_t)nsecs * EXTAL_CLOCK_MHZ / 1000;

    while ((clint_get_cycle() - start) < cycles);
}

void soc_systick_set_next_time(uint64_t expires)
{
    uint64_t now = systick_get_time_us();
    uint64_t usec = expires - now;
    uint32_t delta_count = 0;

    if (now >= expires) {
        usec = 1;
    }

    if (usec < OST_TIMER_MAX_DELTA_US) {
        delta_count = usec * EXTAL_CLOCK_MHZ;
    } else {
        delta_count = OST_TIMER_MAX_VALUE;
    }

    tcu_write_reg(TCU_OSTFULL, delta_count);
    tcu_ost_start();

    return;
}

void soc_systick_set_event_callback(systick_event_handler_t callback)
{
    clint_callback_handler = callback;
}

void soc_systick_init(void)
{
    /* G32使用tcu ost */
    tcu_ost_init();

    is_inited = 1;
}
