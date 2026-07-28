#include <spinlock.h>
#include <driver/clk.h>
#include <soc/cpm.h>
#include <soc/base.h>
#include <soc/extal.h>
#include <common.h>

#include "soc/clk.h"

static DEFINE_SPINLOCK(cpm_pll_lock);

struct pll_rate_setting {
    unsigned long rate;
    unsigned int m;
    unsigned int n;
    unsigned int od1;
    unsigned int od0;
};

#define PLL_RATE(_rate, _m, _n, _od1, _od0) \
{ \
    .rate = _rate, \
    .m = _m, \
    .n = _n, \
    .od1 = _od1, \
    .od0 = _od0, \
}

struct pll_rate_setting pll_rate_table[] = {
    PLL_RATE(1800000000, 75, 1, 1, 1),
    PLL_RATE(1600000000, 200, 3, 1, 1),
    PLL_RATE(1500000000, 125, 2, 1, 1),
    PLL_RATE(1404000000, 117, 2, 1, 1),
    PLL_RATE(1400000000, 175, 3, 1, 1),
    PLL_RATE(1392000000, 58, 1, 1, 1),
    PLL_RATE(1296000000, 54, 1, 1, 1),
    PLL_RATE(1200000000, 50, 1, 1, 1),
    PLL_RATE(1176000000, 49, 1, 1, 1),
    PLL_RATE(1148000000, 287, 3, 1, 2),
    PLL_RATE(1134000000, 189, 2, 1, 2),
    PLL_RATE(1120000000, 140, 3, 1, 1),
    PLL_RATE(1092000000, 91, 1, 1, 2),
    PLL_RATE(1064000000, 133, 3, 1, 1),
    PLL_RATE(1050000000, 175, 2, 1, 2),
    PLL_RATE(1036000000, 259, 3, 1, 2),
    PLL_RATE(1008000000, 42, 1, 1, 1),
    PLL_RATE(1000000000, 125, 3, 1, 1),
    PLL_RATE(980000000, 245, 3, 1, 2),
    PLL_RATE(966000000, 161, 2, 1, 2),
    PLL_RATE(952000000, 119, 3, 1, 1),
    PLL_RATE(924000000, 77, 1, 1, 2),
    PLL_RATE(900000000, 75, 1, 1, 2),
    PLL_RATE(896000000, 112, 3, 1, 1),
    PLL_RATE(882000000, 147, 2, 1, 2),
    PLL_RATE(868000000, 217, 3, 1, 2),
    PLL_RATE(840000000, 35, 1, 1, 1),
    PLL_RATE(812000000, 203, 3, 1, 2),
    PLL_RATE(800000000, 100, 1, 1, 3),
    PLL_RATE(798000000, 133, 2, 1, 2),
    PLL_RATE(784000000, 98, 1, 1, 3),
    PLL_RATE(756000000, 63, 1, 1, 2),
    PLL_RATE(728000000, 91, 1, 1, 3),
    PLL_RATE(714000000, 119, 2, 1, 2),
    PLL_RATE(700000000, 175, 2, 1, 3),
    PLL_RATE(693000000, 231, 4, 1, 2),
    PLL_RATE(672000000, 28, 1, 1, 1),
    PLL_RATE(672000000, 28, 1, 1, 1),
    PLL_RATE(651000000, 217, 4, 1, 2),
    PLL_RATE(630000000, 105, 2, 1, 2),
    PLL_RATE(609000000, 203, 4, 1, 2),
    PLL_RATE(588000000, 49, 1, 1, 2),
    PLL_RATE(567000000, 189, 2, 1, 4),
    PLL_RATE(546000000, 91, 1, 1, 4),
    PLL_RATE(532000000, 133, 2, 1, 3),
    PLL_RATE(525000000, 175, 2, 1, 4),
    PLL_RATE(518000000, 259, 3, 1, 4),
    PLL_RATE(504000000, 42, 1, 1, 2),
    PLL_RATE(490000000, 245, 3, 1, 4),
    PLL_RATE(483000000, 161, 2, 1, 4),
    PLL_RATE(476000000, 119, 2, 1, 3),
    PLL_RATE(462000000, 77, 1, 1, 4),
    PLL_RATE(448000000, 56, 1, 1, 3),
    PLL_RATE(441000000, 147, 2, 1, 4),
    PLL_RATE(434000000, 217, 3, 1, 4),
    PLL_RATE(420000000, 35, 1, 1, 2),
};

int pll_set_rate(struct clk *clk, unsigned long rate)
{
    int found = 0;
    unsigned int val, i;
    int CPXPCR = clk->type_value;
    unsigned int m = 0, n = 0, od0 = 0, od1 = 0;
    unsigned int timeout = 0x1ffff;

    for (i = 0; i < ARRAY_SIZE(pll_rate_table); i++) {
        if (rate == pll_rate_table[i].rate) {
            m = pll_rate_table[i].m;
            n = pll_rate_table[i].n;
            od0 = pll_rate_table[i].od0;
            od1 = pll_rate_table[i].od1;
            found = 1;
        }
    }
    if (!found) {
        printf("Error: Rate not support!\n");
        return -1;
    }
    val = (m << 20) | (n << 14) | (od1 << 11) | (od0 << 8) | 1;
    cpm_outl(val, CPXPCR);

    clk->rate = rate;

    while(!(cpm_inl(CPXPCR) & (1 << 3)) && timeout--);
    if (timeout == 0) {
        debug("wait pll stable timeout!");
        return -1;
    }

    return 0;
}

unsigned long pll_get_rate(struct clk *clk)
{
    unsigned long cpxpcr;
    unsigned long m, n, od0, od1;
    unsigned long rate;
    unsigned long flags;
    struct clk *parent = get_clk_from_id(clk->parent);

    spin_lock_irqsave(&cpm_pll_lock, flags);

    cpxpcr = cpm_inl(clk->type_value);
    if (cpxpcr & 1) {
        if (clk->is_init_enabled == 2)
            clk->is_init_enabled = 1;
        m = ((cpxpcr >> 20) & 0xFFF);
        n = ((cpxpcr >> 14) & 0x3f);
        od1 = ((cpxpcr >> 11) & 0x7);
        od0 = ((cpxpcr >> 8) & 0x7);

        unsigned long fbdiv = m;
        unsigned long refdiv = n;
        unsigned long postdiv2 = od0;
        unsigned long postdiv1 = od1;
        unsigned long fvco = (parent->rate / 1000) * fbdiv / refdiv;
        rate = fvco / postdiv2 / postdiv1;
    } else {
        if (clk->is_init_enabled == 2)
            clk->is_init_enabled = 0;

        rate = 0;
    }

    spin_unlock_irqrestore(&cpm_pll_lock, flags);

    return rate * 1000;
}

static const unsigned int rosc_rates[] = {
    16 * 1000 * 1000, 24 * 1000 * 1000, 32 * 1000 * 1000, 48 * 1000 * 1000
};

int ext_set_rate(struct clk *clk, unsigned long rate)
{
    if (clk->id != CLK_ID_EXT2) {
        printf("Error: %s not support change rate!\n", clk->name);
        return -1;
    }

    unsigned long roscc = aon_inl(AON_ROSCC);

    int i;
    int found = 0;
    for (i = 0; i < ARRAY_SIZE(rosc_rates); i++)
        if (rate == rosc_rates[i]) {
            found = 1;
            break;
        }

    if (!found) {
        printf("Error: Rate not support!\n");
        return -1;
    }

    roscc &= ~(0x3 << 23);
    roscc |= (i << 23);

    aon_outl(roscc, AON_ROSCC);

    return 0;
}

unsigned long ext_get_rate(struct clk *clk)
{
    switch (clk->id)
    {
    case CLK_ID_EXT0: {
        unsigned long roscc = aon_inl(AON_ROSCC);
        unsigned int sel = get_bit_field(&roscc, 29, 30);
        unsigned long rate = JZ_EXTAL_RTC;
        if (sel == 0)
            rate = ext_get_rate(get_clk_from_id(CLK_ID_EXT2)) / 512;
        else if (sel == 1)
            rate = JZ_EXTAL / 512;
        return rate;
    }

    case CLK_ID_EXT1:
        return JZ_EXTAL;

    case CLK_ID_EXT2: {
        unsigned long roscc = aon_inl(AON_ROSCC);

        int fsel = get_bit_field(&roscc, 23, 25);
        return (fsel < 4) ? rosc_rates[fsel] : 0;
    }

    default:
        break;
    }

    return 0;
}

void init_ext_pll(struct clk *clk)
{
    switch (clk->id) {
    case CLK_ID_EXT0:
        clk->is_init_enabled = 1;
        clk->rate = ext_get_rate(clk);
        break;

    case CLK_ID_EXT1:
        clk->is_init_enabled = 1;
        clk->rate = ext_get_rate(clk);
        break;

    case CLK_ID_EXT2:
        clk->is_init_enabled = 1;
        clk->rate = ext_get_rate(clk);
        break;

    case CLK_ID_APLL:
    case CLK_ID_MPLL:
        clk->parent = CLK_ID_EXT1;
        clk->is_init_enabled = 2;
        clk->rate = pll_get_rate(clk);
        break;

    case CLK_ID_APLL_DIV8:
        clk->parent = CLK_ID_EXT1;
        clk->is_init_enabled = 2;
        clk->rate = pll_get_rate(clk) / 8;
        break;
    }
}
