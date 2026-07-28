#include <spinlock.h>

#include <asm/cacheops.h>
#include <soc/cpm.h>
#include <driver/cache.h>
#include <soc/base.h>
#include <soc/extal.h>
#include <common.h>
#include "spinlock.h"
#include "soc/clk.h"

static DEFINE_SPINLOCK(cpm_cpccr_lock);
#define CPCCR_CLK_ID_NO(id)             ((id) - CLK_ID_CPPCR - 1)

struct cpccr_clk {
    short off, sel, ce;
};

static struct cpccr_clk cpccr_clks[] = {
#define CPCCR_CLK(N,O,D,E)            \
    [N] = { .off = O, .sel = D, .ce = E}
    CPCCR_CLK(SCLKA, -1, 30, 23),
    CPCCR_CLK(CDIV,   0, 28, 22),
    CPCCR_CLK(L2CDIV, 4, 28, 22),
    CPCCR_CLK(H0DIV,  8, 26, 21),
    CPCCR_CLK(H2DIV, 12, 24, 20),
    CPCCR_CLK(PDIV,  16, 24, 20),
    CPCCR_CLK(PMDIV,  4,  9,  8),
    CPCCR_CLK(HMDIV,  0,  9,  8),
#undef CPCCR_CLK
};

static unsigned int cpccr_selector[4] = {0, CLK_ID_SCLKA, CLK_ID_MPLL, CLK_ID_EXT2};
static unsigned int sclka_selector[4] = {0, CLK_ID_EXT2, CLK_ID_APLL, CLK_ID_EXT1};

static inline void cpm_set_cpccrx(int id, unsigned long cpccrx)
{
    if (id == HMDIV || id == PMDIV)
        cpm_outl(cpccrx, CPM_CPCCR1);
    else
        cpm_outl(cpccrx, CPM_CPCCR);
}

static inline unsigned long cpm_get_cpccrx(int id)
{
    if (id == HMDIV || id == PMDIV)
        return cpm_inl(CPM_CPCCR1);
    else
        return cpm_inl(CPM_CPCCR);
}

static inline void cpm_set_parent(int id, int parent)
{
    unsigned long cpccrx = cpm_get_cpccrx(id);

    /* 设置时钟源 */
    set_bit_field(&cpccrx, cpccr_clks[id].sel, cpccr_clks[id].sel + 1, parent);
    cpm_set_cpccrx(id, cpccrx);
}

static inline int caculate_div(int id, unsigned int rate, unsigned int parent_rate)
{
    /* 通过parent_rate 和rate算出最合理的分频值 */
    int div;
    unsigned int max_div = 1 << 4;

    for (div = 1; div <= max_div; div++) {
        if (parent_rate / div <= rate)
            break;
    }

    if (div > max_div)
        div = max_div;

    return div;
}

static inline int cpccr_get_div(int id, unsigned long cpccrx)
{
    int div = get_bit_field(&cpccrx, cpccr_clks[id].off, cpccr_clks[id].off + 3);

    return (div + 1);
}

static inline void cpccr_set_div(int id, int div, unsigned long *cpccrx)
{
    /* 如果分频值没有变化，就不用设置 */
    if (cpccr_get_div(id, *cpccrx) == div)
        return;

    int xdiv = div - 1;
    set_bit_field(cpccrx, cpccr_clks[id].off, cpccr_clks[id].off + 3, xdiv);
    if (id != SCLKA)
        set_bit_field(cpccrx, cpccr_clks[id].ce, cpccr_clks[id].ce, 1);
    cpm_set_cpccrx(id, *cpccrx);

    if (id != SCLKA)
        set_bit_field(cpccrx, cpccr_clks[id].ce, cpccr_clks[id].ce, 0);
    cpm_set_cpccrx(id, *cpccrx);
}

static inline struct clk *to_parent_clk(int id, unsigned long cpccrx)
{
    int sel = (cpccrx >> cpccr_clks[id].sel) & 0x3;
    int parent_id = cpccr_selector[sel];
    if (id == SCLKA)
        parent_id = sclka_selector[sel];

    return get_clk_from_id(parent_id);
};

unsigned long cpccr_get_rate(struct clk *clk)
{
    int id = CPCCR_CLK_ID_NO(clk->id);
    struct clk *parent = get_clk_from_id(clk->parent);
    unsigned long cpccrx = cpm_get_cpccrx(id);
    int cpccr_type = clk->type_value;
    unsigned int rate = 0;
    int v;

    if (clk->parent)
        rate = clk_get_rate(parent);

    if (cpccr_type == SCLKA)
        v = 1;
    else
        v = ((cpccrx >> cpccr_clks[cpccr_type].off) & 0xf) + 1;

    return (rate / v);
}

unsigned long cpccr_set_rate(struct clk *clk, unsigned long rate)
{
    int id = CPCCR_CLK_ID_NO(clk->id);
    unsigned long flags;
    struct clk *parent = get_clk_from_id(clk->parent);

    spin_lock_irqsave(&cpm_cpccr_lock, flags);
    unsigned long cpccrx = cpm_get_cpccrx(id);
    int div = caculate_div(id, rate, parent->rate);

    cpccr_set_div(id, div, &cpccrx);

    clk->rate = parent->rate / div;

    spin_unlock_irqrestore(&cpm_cpccr_lock, flags);

    return 0;
}

struct clk *cpccr_get_parent(struct clk *clk)
{
    int id = CPCCR_CLK_ID_NO(clk->id);
    unsigned long cpccrx = cpm_get_cpccrx(id);

    return to_parent_clk(id, cpccrx);
}

int cpccr_set_parent(struct clk *clk, struct clk *parent)
{
    unsigned long flags;
    int clk_sel;
    int cpccr_type = clk->type_value;
    if (cpccr_type == SCLKA) {
        for (clk_sel = 0; clk_sel < 4; clk_sel++)
            if (sclka_selector[clk_sel] == parent->id)
                break;
    } else {
        for (clk_sel = 0; clk_sel < 4; clk_sel++) {
            if (clk_sel == 3 && cpccr_type != CDIV)
                break;
            if (sclka_selector[clk_sel] == parent->id)
                break;
        }
    }

    if (clk_sel >= 4)
        return -EINVAL;

    spin_lock_irqsave(&cpm_cpccr_lock, flags);

    /* 设置时钟源 */
    cpm_set_parent(CPCCR_CLK_ID_NO(clk->id), clk_sel);
    clk->parent = parent->id;

    spin_unlock_irqrestore(&cpm_cpccr_lock, flags);

    /* 更新时钟频率 */
    cpccr_set_rate(clk, clk->rate);

    return 0;
}

void init_cpccr_clk(struct clk *clk)
{
    int id = CPCCR_CLK_ID_NO(clk->id);
    int cpccr_type = clk->type_value;

    unsigned long cpccrx = cpm_get_cpccrx(id);
    int sel = (cpccrx >> cpccr_clks[cpccr_type].sel) & 0x3;

    if (cpccr_type != SCLKA) {
        if (cpccr_selector[sel] != 0) {
            clk->parent = cpccr_selector[sel];
            clk->is_init_enabled = 1;
        }
    } else {
        int gate = (cpccrx >> cpccr_clks[SCLKA].ce) & 0x1;
        if (sclka_selector[sel] != 0) {
            clk->parent = sclka_selector[sel];
            if (!gate)
                clk->is_init_enabled = 1;
        }
    }

    clk->rate = cpccr_get_rate(clk);
}
