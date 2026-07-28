#include <common.h>

#ifndef CONFIG_TO_BE_CHANGE
#include <driver/clk.h>
#endif

#include <driver/dtrng.h>
#include <soc/base.h>

/* DTRNG register */
#define DTRNG_CFG       0x00
#define DTRNG_RANDOMNUM 0x04
#define DTRNG_STAT      0x08

#define DTRNG_CFG_LINE_EN   16, 25
#define DTRNG_CFG_RDY_CLR   12, 12
#define DTRNG_CFG_INT_MASK  11, 11
#define DTRNG_CFG_DIV_NUM    1, 10
#define DTRNG_CFG_GEN_EN     0, 0

#define DTRNG_STAT_RANDOM_RDY 0, 0

#define DTRNG_REG_BASE KSEG1ADDR(DTRNG_IOBASE)

#define DTRNG_ADDR(reg) ((volatile unsigned long *)(DTRNG_REG_BASE + reg))

static inline void dtrng_write_reg(unsigned int reg, unsigned int value)
{
    *DTRNG_ADDR(reg) = value;
}

static inline unsigned int dtrng_read_reg(unsigned int reg)
{
    return *DTRNG_ADDR(reg);
}

static inline void dtrng_set_bits(unsigned int reg, int start, int end, unsigned int val)
{
    set_bit_field_v(DTRNG_ADDR(reg), start, end, val);
}

static inline unsigned int dtrng_get_bits(unsigned int reg, int start, int end)
{
    return get_bit_field_v(DTRNG_ADDR(reg), start, end);
}

/* DTRNG soc api */
#define DTRNG_FREQ_DIV 0
#define DTRNG_TIMEOUT_US 1000

static DEFINE_SPINLOCK(lock);

#ifndef CONFIG_TO_BE_CHANGE
static struct clk *dtrng_clk;
#endif

static void dtrng_enable(void)
{
    dtrng_set_bits(DTRNG_CFG, DTRNG_CFG_GEN_EN, 1);
}

static void dtrng_disable(void)
{
    dtrng_set_bits(DTRNG_CFG, DTRNG_CFG_GEN_EN, 0);
}

static void dtrng_clean_flag(void)
{
    dtrng_set_bits(DTRNG_CFG, DTRNG_CFG_RDY_CLR, 1);
    dtrng_set_bits(DTRNG_CFG, DTRNG_CFG_RDY_CLR, 0);
}

unsigned int soc_dtrng_read_random_data(void)
{
    unsigned long flags;
    unsigned long long now = systick_get_time_us();

    spin_lock_irqsave(&lock, flags);

    dtrng_enable();
    dtrng_clean_flag();

    /* wait random_rdy */
    while (!dtrng_get_bits(DTRNG_STAT, DTRNG_STAT_RANDOM_RDY)) {
        if (systick_get_time_us() - now >= DTRNG_TIMEOUT_US) {
            dtrng_disable();
            spin_unlock_irqrestore(&lock, flags);
            jz_log_error(JZ_DTRNG_MOD, "get random data timeout\n");
            return (unsigned int)-1;
        }
    }

    unsigned int value = dtrng_read_reg(DTRNG_RANDOMNUM);
    dtrng_disable();

    spin_unlock_irqrestore(&lock, flags);

    return value;
}

void soc_dtrng_init(void)
{
#ifndef CONFIG_TO_BE_CHANGE
    dtrng_clk = clk_get("gate_dtrng");
    assert(dtrng_clk);
    clk_enable(dtrng_clk);
#endif

    dtrng_set_bits(DTRNG_CFG, DTRNG_CFG_LINE_EN, 0x3FF);
    dtrng_set_bits(DTRNG_CFG, DTRNG_CFG_DIV_NUM, DTRNG_FREQ_DIV);
    dtrng_set_bits(DTRNG_CFG, DTRNG_CFG_INT_MASK, 1);
}

void soc_dtrng_deinit(void)
{
#ifndef CONFIG_TO_BE_CHANGE
    clk_disable(dtrng_clk);
    clk_put(dtrng_clk);
#endif
}
