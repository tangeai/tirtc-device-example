#include <common.h>
#include <lds_symbol.h>
#include <func_symbol.h>
#include <module.h>

#define IS_C_INSN(opcode) ((opcode & 0x3) != 0x3)

struct riscv_frame_info {
    const char *name;        /* func name */
    void *func;              /* pc address */
    unsigned long func_size; /* analyze from start to current PC offset */
    long stack_size;         /* stack size */
    long ra_offset;          /* ra:next pc in stack offset */
};

#ifndef CONFIG_TO_BE_CHANGE
#ifdef CONFIG_OS
extern void end_of_thread(void);
#endif
#endif

__weak int is_kernel_code_address(unsigned long addr)
{
    // debug("addr: %x, <%p, %p>\n", addr, &__text_start, &__text_end);
    return addr >= (unsigned long)&__text_start && addr < (unsigned long)&__text_end;
}

__weak int is_code_address(unsigned long addr)
{
    if (is_kernel_code_address(addr))
        return 1;
#ifdef CONFIG_OS_MODULE
    if (is_in_module(addr))
        return 1;
#endif
    return 0;
}

__weak int is_stack_address(unsigned long addr)
{
    unsigned long start = (unsigned long)&_user_stack_start;
    unsigned long end = (unsigned long)&_user_heap_end;

    return addr >= start && addr < end;
}

static inline int is_jump_ins(unsigned int insn, int is_compressed)
{
    if (is_compressed) {
        return (insn & 0xE003) == 0xA001 || (insn & 0xE003) == 0x2001;
    }
    return ((insn & 0x7F) == 0x6F) || ((insn & 0x707F) == 0x67);
}

static inline int is_sp_adjust_ins(unsigned int insn, int is_compressed)
{
    if (is_compressed) {
        return ((insn & 0xEF83) == 0x0101);
    }
    return ((insn & 0x000FFFFF) == 0x00010113);
}

static int decode_imm(unsigned int insn, int is_compressed)
{
    if (is_compressed) {
        int n = ((insn >> 2) & 0x1F);
        return (insn & 0x1000) ? -(32 - n) : n;
    }
    return ((int)insn) >> 20;
}

static inline int is_ra_save_ins(unsigned int insn, int is_compressed)
{
    if (is_compressed) {
        return ((insn & 0xE07F) == 0xC006);
    }
    return ((insn & 0x01FFF07F) == 0x00112023);
}

static int decode_store_offset(unsigned int insn, int is_compressed)
{
    if (is_compressed) {
        return (((insn >> 1) & 0xC0) | ((insn >> 7) & 0x3C));
    }
    return ((insn >> 20) & 0xFE0) | ((insn >> 7) & 0x1F);
}

static int get_frame_info(struct riscv_frame_info *info)
{
    unsigned short *ip16 = (unsigned short *)info->func;
    unsigned int *ip32 = (unsigned int *)info->func;
    unsigned max_bytes = info->func_size ?: 512U; /* unknown function size */
    unsigned bytes_parsed = 0;

    info->ra_offset = -1;
    info->stack_size = 0;

    if (!info->func)
        goto err;

    // debug("info->func_size: %d, max_bytes: %d\n", info->func_size, max_bytes);
    while (bytes_parsed < max_bytes) {
        unsigned int insn;
        int is_compressed = IS_C_INSN(*ip16);
        if (is_compressed) {
            insn = *ip16 & 0xFFFF;
            // debug("unwind: *(%p)=0x%04lx(compress)\n", ip16, insn);
        } else {
            insn = *ip32;
            // debug("unwind: *(%p)=0x%08lx(standard)\n", ip32, insn);
        }

        if (is_jump_ins(insn, is_compressed)) {
            debug("Stop analysis at jump instruction\n");
            break;
        }

        if (!info->stack_size) {
            if (is_sp_adjust_ins(insn, is_compressed)) {
                int imm = decode_imm(insn, is_compressed);
                if (imm < 0)
                    info->stack_size = -imm;
                goto loop;
            }
        }

        if (info->ra_offset == -1 && is_ra_save_ins(insn, is_compressed)) {
            info->ra_offset = decode_store_offset(insn, is_compressed) / sizeof(long);
            break;
        }
    loop:
        if (is_compressed) {
            ip16++;
            bytes_parsed += 2;
            if (!IS_C_INSN(*ip16))
                ip32 = (unsigned int *)ip16;
        } else {
            ip32++;
            bytes_parsed += 4;
            ip16 = (unsigned short *)ip32;
        }
    }

    // debug("unwind: sp_off=0x%lx(%ld), ra_off=%ld\n", -info->stack_size, -info->stack_size, info->ra_offset);

    if (info->stack_size && info->ra_offset >= 0) /* nested */
        return 0;

    if (info->ra_offset < 0) /* leaf */
        return 1;

err:
    return -1;
}

static const char *kernel_address_lookup(unsigned long addr,
        unsigned long *size, unsigned long *offset, char **modname)
{
    struct func_symbol *symbol = func_symbol_lookup(addr);
    if (!symbol) {
        debug("unwind: pc out of range of kernel symbol\n");
        return NULL;
    }

    if (modname)
        *modname = (char *)symbol->name;

    if (offset)
        *offset = addr - symbol->addr;

    // debug("offset(%lx)(%lx): addr(%lx) - symbol->addr(%lx)\n", *offset, addr - symbol->addr, addr, symbol->addr);
    return symbol->name;
}

/*
 * Lookup an address but don't bother to find any names.
 */
static const char *kallsyms_lookup_size_offset(unsigned long addr,
        unsigned long *symbolsize, unsigned long *offset)
{
    if (is_kernel_code_address(addr))
        return kernel_address_lookup(addr, symbolsize, offset, NULL);
#ifdef CONFIG_OS_MODULE
    if (is_in_module(addr))
        return module_address_lookup(addr, symbolsize, offset, NULL);
#endif
    return NULL;
}

static unsigned long unwind_stack_by_address(unsigned long *sp,
                          unsigned long pc,
                          unsigned long *ra)
{
    struct riscv_frame_info info;
    unsigned long size, ofs;
    int leaf;

    if (!kallsyms_lookup_size_offset(pc, &size, &ofs)) {
        debug("unwind: pc out of range. not find symbol\n");
        return 0;
    }

    info.func = (void *)(pc - ofs);
    // info.func_size = (ofs < 8) ? ofs : ofs - 8; /* analyze from start to ofs */
    info.func_size = ofs; /* analyze from start to ofs */
    leaf = get_frame_info(&info);
    if (leaf < 0) {
        debug("unwind: function out of range\n");
        return 0;
    }

    if (leaf) {
        /*
         * For some extreme cases, get_frame_info() can
         * consider wrongly a nested function as a leaf
         * one. In that cases avoid to return always the
         * same value.
         */
        pc = pc != *ra ? *ra : 0;
    } else {
        pc = ((unsigned long *)(*sp))[info.ra_offset];
    }

    *sp += info.stack_size;
    *ra = 0;
    return pc;
}

static unsigned long unwind_stack(unsigned long *sp,
        unsigned long pc, unsigned long *ra)
{
    // debug("====== unwind: *sp= %08lx, pc= %08lx, *ra= %08lx ======\n", *sp, pc, *ra);

    if (!is_stack_address(*sp)) {
        debug("unwind: *sp out of range 1\n");
        return 0;
    }

    return unwind_stack_by_address(sp, pc, ra);
}

static int print_ip_sym(unsigned long pc, int is_print)
{
    const char *func_name;
    unsigned long offset;

    func_name = kallsyms_lookup_size_offset(pc, NULL, &offset);
    if (!func_name) {
        debug("[<%08lx>] ???\n", pc);
        return -1;
    }

    if (is_print) {
        if (offset < 8) {
            printf("[<%p>] %s+0x%lx\n", (void *)(pc - offset), func_name, offset);
        } else {
            printf("[<%p>] %s+0x%lx\n", (void *)(pc - offset), func_name, offset - 8);
        }
    }

    return 0;
}

void show_raw_backtrace(unsigned long sp, unsigned long pc, unsigned long ra)
{
    unsigned long save_pc = -1;
    unsigned long save_ra = -1;
    unsigned long save_sp = -1;
    int is_print = 1;

    printf("Call Trace:\n");
    do {
        save_pc = pc;
        save_ra = ra;
        save_sp = sp;
        // debug("save_sp: %08lx, save_pc: %08lx, save_ra: %08lx\n", save_sp, save_pc, save_ra);

        print_ip_sym(pc, is_print);
        pc = unwind_stack(&sp, pc, &ra);
        is_print = save_pc != pc;
        // debug("sp: %08lx, pc: %08lx, ra: %08lx\n\n", sp, pc, ra);
        if (save_pc == pc && save_ra == ra && save_sp == sp) {
            printf("can't unwind stack anymore\n");
            break;
        }

#ifndef CONFIG_TO_BE_CHANGE
#ifdef CONFIG_OS
        if (ra == (unsigned long)end_of_thread ||
            pc == (unsigned long)end_of_thread)
            break;
#endif
#endif

    } while (pc);
}

void show_stacktrace(unsigned long _sp, unsigned long pc, unsigned long ra)
{
    int i;
    unsigned long *sp = (unsigned long *)_sp;

    printf("Stack :\n");
    if ((unsigned long)sp & 3) {
        printf(" Not aligned!\n");
        return;
    }

    i = 0;
    while (is_stack_address((unsigned long)sp + 32)) {
        dump_mem32(sp, 32, 8);
        sp += 8;
        if (++i == 4)
            break;
    }

    show_raw_backtrace(_sp, pc, ra);
}

static inline unsigned long get_sp(void)
{
    unsigned long reg;

    __asm__ volatile (
        "mv %0, sp"
        : "=r" (reg)
        :
    );

    return reg;
}

static inline unsigned long get_ra(void)
{
    unsigned long reg;

    __asm__ volatile (
        "mv %0, ra"
        : "=r" (reg)
        :
    );

    return reg;
}

void dump_stack(void)
{
LLLL:
    show_stacktrace(get_sp(), (unsigned long)&&LLLL, get_ra());
}
EXPORT_SYMBOL(dump_stack);
