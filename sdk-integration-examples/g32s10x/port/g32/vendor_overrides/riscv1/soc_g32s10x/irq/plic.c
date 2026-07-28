#include "soc/riscv.h"
#include <stdio.h>
#include <stdint.h>
#include <spinlock.h>
#include <common.h>
#include "io.h"
#include "assert.h"
#include "soc/plic.h"
#define PLIC_DEBUG

/* 减少 中断嵌套响应时间(未严格进行测试, 后续 优化中断代码 将重新整理) */
// #define PLIC_FAST_ENTRY

//static DEFINE_SPINLOCK(plic_lock);
#define SHARED_MEM __attribute__((section(".shared_mem")))

spinlock_t plic_lock = (spinlock_t){
    .count = 0, .is_irq = 0, .recursive = 0, .re_count = 0, .name = "plic_lock",
    .thread = NULL, .arch_lock.lock = 0 };
// Function to initialize the PLIC
void plic_init(void)
{
    // plic_src_regs->config._b.intsrc=PLIC_IRQ_ID_63;
    unsigned int hard_id = read_csr(mhartid);
    volatile plic_config_reg_t *ConfigReg = (volatile plic_config_reg_t *)PLIC_CONFIG_ADDR;
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    ConfigReg->_b.intsrc=PLIC_NUM_INT_SRC;
    ConfigReg->_b.intpri=PLIC_INT_PRIORITY_HIGHEST;

    // Set the maximum priority for all interrupts
    for (int i = 1; i < PLIC_NUM_INT_SRC; i++) {
        volatile plic_prio_reg_t *prio_n = (volatile plic_prio_reg_t *)PLIC_PRIO_ADDR(i);
        prio_n->_w = PLIC_INT_PRIORITY_LOWEST;
    }

    // Clear all pending interrupts
    volatile plic_ip_reg_t *ip_l = (volatile plic_ip_reg_t *)PLIC_IP_L_ADDR;
    volatile plic_ip_reg_t *ip_h = (volatile plic_ip_reg_t *)PLIC_IP_H_ADDR;
    ip_l->_w = 0;
    ip_h->_w = 0;

    // Disable all interrupts for all targets

    volatile plic_ie_reg_t *ie_l = (volatile plic_ie_reg_t *)PLIC_IEn_L_ADDR(hard_id);
    volatile plic_ie_reg_t *ie_h = (volatile plic_ie_reg_t *)PLIC_IEn_H_ADDR(hard_id);
    ie_l->_w = 0;
    ie_h->_w = 0;

    // Set the priority threshold for all targets to the lowest priority
    volatile plic_threshold_reg_t *threshold = (volatile plic_threshold_reg_t *)PLIC_THRESHOLD_ADDR(hard_id);
    threshold->_w = PLIC_INT_PRIORITY_LOWEST;

#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    debug("hard_id:%d plicconfig_RegVal:0x%x max_int_prio:%d max_int_src:%d \n",hard_id, (unsigned int)ConfigReg->_w, ConfigReg->_b.intpri,  ConfigReg->_b.intsrc);

}


/* Function to set the priority of an interrupt */
bool plic_set_priority(plic_irq_id_e irq, plic_int_priority_e priority) {

    if (irq == 0){
        printf(" Warning: irq is ZERO , do not write\n");
        return false;
    }

    if( (irq < PLIC_IRQ_ID_1) || (irq >  PLIC_IRQ_ID_63)){
        printf(" Error: plic priority not set! irq:%d priority:%d \n", irq, priority);
        return false;
    }
    if (priority == PLIC_INT_PRIORITY_0){
        printf(" Warning: priority set to zero, disabling the interrupt! irq:%d priority:%d \n", irq, priority);
    }
    volatile plic_prio_reg_t *prio_n = (volatile plic_prio_reg_t *)PLIC_PRIO_ADDR(irq);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    prio_n->_b.prio = priority & 0xF;
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    // plic_register_dump(read_csr(mhartid));
    return true;

}

uint32_t plic_get_priority(plic_irq_id_e irq) {
    if((irq < PLIC_IRQ_ID_1) || (irq >  PLIC_IRQ_ID_63)){
        printf(" Error: irq %d is out of range\n", irq);
    }
    volatile plic_prio_reg_t *prio_n = (volatile plic_prio_reg_t *)PLIC_PRIO_ADDR(irq);
    uint32_t prio = 0;
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;

    spin_lock_irqsave(&plic_lock, flags);
#endif
    prio = prio_n->_b.prio;
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif

    return prio;
}

/* Function to enable an interrupt for a specific target */
void plic_enable_irq(plic_int_target_e target, plic_irq_id_e irq)
{
    volatile plic_ie_reg_t *ie_l = (volatile plic_ie_reg_t *)PLIC_IEn_L_ADDR(target);
    volatile plic_ie_reg_t *ie_h = (volatile plic_ie_reg_t *)PLIC_IEn_H_ADDR(target);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
#endif

    if ( !(target < PLIC_NUM_TARGETS && irq < PLIC_IRQ_ID_MAX && irq > PLIC_IRQ_ID_NONE) ) {
        printf(" Error: function argument value out of range\n");
        return;
    }
    if (irq == 0){
        printf(" Error: trying to set interrupt zero, not allowed\n");
        return;
    }
#ifndef PLIC_FAST_ENTRY
    spin_lock_irqsave(&plic_lock, flags);
#endif
    if (irq < 32) {
        ie_l->_w |= (1 << irq);
    } else {
        ie_h->_w |= (1 << (irq - 32));
    }
    // plic_register_dump(read_csr(mhartid))
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif

}

/* Function to disable an interrupt for a specific target */
bool plic_disable_irq(plic_int_target_e target, plic_irq_id_e irq)
{
    if ( !(target < PLIC_NUM_TARGETS && irq < PLIC_IRQ_ID_MAX && irq > PLIC_IRQ_ID_NONE) ) {
        printf(" Error: unable to disable irq:%d for target:%d, function argument value out of range\n",target,irq);
        return false;
    }
    volatile plic_ie_reg_t *ie_l = (volatile plic_ie_reg_t *)PLIC_IEn_L_ADDR(target);
    volatile plic_ie_reg_t *ie_h = (volatile plic_ie_reg_t *)PLIC_IEn_H_ADDR(target);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;

    spin_lock_irqsave(&plic_lock, flags);
#endif

    if (irq < 32) {
        ie_l->_w &= ~(1 << irq);
    } else {
        ie_h->_w &= ~(1 << (irq - 32));
    }
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
     // plic_register_dump(read_csr(mhartid));
     return true;
}

/* Function to set the interrupt threshold for a specific target */
bool plic_set_threshold(plic_int_target_e target, plic_int_priority_e threshold) {
    if ( (target >= PLIC_NUM_TARGETS || threshold > PLIC_INT_PRIORITY_HIGHEST) ) {
        printf(" Error: unable to set threshold %d for target %d, function argument value out of range\n",threshold, target);
        return false;
    }
    volatile plic_threshold_reg_t *thresholdreg = (volatile plic_threshold_reg_t *)PLIC_THRESHOLD_ADDR(target);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    thresholdreg->_b.threshold = threshold;
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    // plic_register_dump(read_csr(mhartid))
    return true;
}

/* Function to set the interrupt threshold for a specific target */
plic_int_priority_e plic_get_threshold(plic_int_target_e target)
{
    if ( target >= PLIC_NUM_TARGETS  ) {
        printf(" Error: unable to get threshold from target %d, function argument value out of range\n", target);
        return false;
    }
    volatile plic_threshold_reg_t *thresholdreg = (volatile plic_threshold_reg_t *)PLIC_THRESHOLD_ADDR(target);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    plic_int_priority_e th =  thresholdreg->_b.threshold;
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    // plic_register_dump(read_csr(mhartid));
    return th;
}

/* Function to claim an interrupt for a specific target */
plic_irq_id_e plic_claim_interrupt(plic_int_target_e target)
{

    if ( (target >= PLIC_NUM_TARGETS ) ) {
        printf(" Error: unable to claim interrupt for target %d, function argument value out of range\n", target);
        return false;
    }
    volatile plic_claim_reg_t *claim = (volatile plic_claim_reg_t *)PLIC_CLAIM_ADDR(target);


#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    uint32_t claimedIrq = claim->_b.id;
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    return claimedIrq;
}

/* Function to complete an interrupt for a specific target */
bool plic_complete_interrupt(plic_int_target_e target, plic_irq_id_e irq)
{
    if ( (target >= PLIC_NUM_TARGETS ) || ( irq > PLIC_IRQ_ID_63 || irq < PLIC_IRQ_ID_1) ) {
        printf(" Error in [%s]: unable to complete IRQ:%d for target %d, function argument value out of range\n",__func__, irq, target);
        return false;
    }
    volatile plic_claim_reg_t *claim = (volatile plic_claim_reg_t *)PLIC_CLAIM_ADDR(target);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif

        /* claim->_b.id = irq;
    * Note: in above code, it wll read the claim register first then clear off the other bits and
    * then write the result back to the claim register,
    * and the read opration has spectial meaning in PLIC, So we don't need read operation here, we only need a write operation.
    * as it will acuire a IRQ and clear the penig bit.
    * */
   claim->_w = irq & PLIC_CLAIM_REG_ID_FIELD_MASK;

#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    return true;
}




/* Function to check if an interrupt irq is pending */
bool plic_is_pending(plic_irq_id_e irq) {
    if((irq < PLIC_IRQ_ID_1) || (irq >  PLIC_IRQ_ID_63)){
        printf(" Error: irq %d is out of range\n", irq);
        return false;
    }

    volatile plic_ip_reg_t *ip_l = (volatile plic_ip_reg_t *)PLIC_IP_L_ADDR;
    volatile plic_ip_reg_t *ip_h = (volatile plic_ip_reg_t *)PLIC_IP_H_ADDR;

    volatile uint32_t pending;
    uint32_t mask;
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    if (irq <= 31) {
        pending = ip_l->_w;
        mask = 1 << irq;
    } else {
        pending = ip_h->_w;
        mask = 1 << (irq - 32);
    }
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif

    return (pending & mask) != 0;
}

void plic_print_pending(void) {
    volatile plic_ip_reg_t *ip_l = (volatile plic_ip_reg_t *)PLIC_IP_L_ADDR;
    volatile plic_ip_reg_t *ip_h = (volatile plic_ip_reg_t *)PLIC_IP_H_ADDR;
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    uint32_t pending_l = ip_l->_w;
    uint32_t pending_h = ip_h->_w;
#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    printf("Pending Interrupts Lower: 0x%08x\n", (unsigned int)pending_l);
    printf("Pending Interrupts High: 0x%08x\n", (unsigned int)pending_h);
}

// void plic_set_pending(plic_irq_id_e irq) {
//     volatile plic_ip_reg_t *ip_l = (volatile plic_ip_reg_t *)PLIC_IP_L_ADDR;
//     volatile plic_ip_reg_t *ip_h = (volatile plic_ip_reg_t *)PLIC_IP_H_ADDR;
//     unsigned long flags;
//     spin_lock_irqsave(&plic_lock, flags);
//     if (irq < 32) {
//         ip_l->_w |= (1 << irq);
//     } else {
//         ip_h->_w |= (1 << (irq - 32));
//     }
//     spin_unlock_irqrestore(&plic_lock, flags);
//     printf("Pending Interrupts Lower: 0x%08x\n", (unsigned int)ip_l->_w);
//     printf("Pending Interrupts High: 0x%08x\n", (unsigned int)ip_h->_w);
// }


#ifdef PLIC_DEBUG


void plic_register_dump(plic_int_target_e core) {
    char buffer[512];  // Allocate a buffer large enough to hold the output
    int offset = 0;


    plic_threshold_reg_t threshold_reg;
    threshold_reg._w = *(volatile uint32_t *)(PLIC_THRESHOLD_ADDR(core));

    // Read MaxPrio from the PLIC priority register
    volatile plic_config_reg_t *ConfigReg = (volatile plic_config_reg_t *)PLIC_CONFIG_ADDR;

    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                        "Core%d, CurrentThreshold: 0x%02x PlicConfig (MaxSrc: %d MaxPrio: %d)\n",
                        core, threshold_reg._b.threshold, ConfigReg->_b.intsrc, ConfigReg->_b.intpri);

    // Print IRQs, PRIO, PEND, and ENAB for the first 32 interrupts
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "IRQs:  ");
    for (int i = 0; i < 32; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2d ", i);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nPRIO:  ");
    for (int i = 0; i < 32; i++) {
        plic_prio_reg_t prio_reg;
        prio_reg._w = *(volatile uint32_t *)(PLIC_PRIO_ADDR(i));
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2x ", prio_reg._b.prio);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nPEND:  ");
    plic_ip_reg_t pending_low;
    pending_low._w = *(volatile uint32_t *)(PLIC_IP_L_ADDR);
    for (int i = 0; i < 32; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2x ", (pending_low._w >> i) & 1);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nENAB:  ");
    plic_ie_reg_t enable_low;
    enable_low._w = *(volatile uint32_t *)(PLIC_IEn_L_ADDR(core));
    for (int i = 0; i < 32; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2x ", (enable_low._w >> i) & 1);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n\n");
    // Print the entire buffer at once
    printf("%s", buffer);
    offset = 0;
    // Print IRQs, PRIO, PEND, and ENAB for the remaining interrupts
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nIRQs:  ");
    for (int i = 32; i <= PLIC_NUM_INT_SRC; i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2d ", i);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nPRIO:  ");
    for (int i = 32; i <= PLIC_NUM_INT_SRC; i++) {
        plic_prio_reg_t prio_reg;
        prio_reg._w = *(volatile uint32_t *)(PLIC_PRIO_ADDR(i));
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2x ", prio_reg._b.prio);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nPEND:  ");
    plic_ip_reg_t pending_high;
    pending_high._w = *(volatile uint32_t *)(PLIC_IP_H_ADDR);
    for (int i = 0; i <= (PLIC_NUM_INT_SRC - 32); i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2x ", (pending_high._w >> i) & 1);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\nENAB:  ");
    plic_ie_reg_t enable_high;
    enable_high._w = *(volatile uint32_t *)(PLIC_IEn_H_ADDR(core));
    for (int i = 0; i <= (PLIC_NUM_INT_SRC - 32); i++) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%2x ", (enable_high._w >> i) & 1);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n\n");

    // Print the entire buffer at once
    printf("%s", buffer);
}
#endif // PLIC_DEBUG

plic_irq_id_e plic_nest_claim(plic_int_target_e target, uint32_t *pCurTH)
{
    volatile plic_claim_reg_t *claim = (volatile plic_claim_reg_t *)PLIC_CLAIM_ADDR(target);
    volatile plic_threshold_reg_t *th = (volatile plic_threshold_reg_t *)PLIC_THRESHOLD_ADDR(target);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    uint32_t claimedIrq = claim->_b.id;
    *pCurTH = th->_b.threshold;
    if (claimedIrq == 0){
#ifndef PLIC_FAST_ENTRY
        spin_unlock_irqrestore(&plic_lock, flags);
#endif
        return claimedIrq;
    }
    volatile plic_prio_reg_t *prio_n = (volatile plic_prio_reg_t *)PLIC_PRIO_ADDR(claimedIrq);
    uint32_t claimedIrqPrio = prio_n->_b.prio;
    th->_b.threshold = claimedIrqPrio;//claimedIrqPrio > target_th ? claimedIrqPrio:target_th;

    /* claim->_b.id = claimedIrq;
    * Note: in above code, it wll read the claim register first then clear off the other bits and
    * then write the result back to the claim register,
    * and the read opration has spectial meaning in PLIC, So we don't need read operation here,
    * as it will acuire a IRQ and clear the penig bit.
    * */
   // claim->_w = claimedIrq & PLIC_CLAIM_REG_ID_FIELD_MASK;

#ifndef PLIC_FAST_ENTRY
    spin_unlock_irqrestore(&plic_lock, flags);
#endif
    return claimedIrq;
}

bool plic_nest_complete(plic_int_target_e target, uint32_t *pSavedTH, plic_irq_id_e irq)
{
    if ( (target >= PLIC_NUM_TARGETS ) || ( irq > PLIC_IRQ_ID_63 || irq < PLIC_IRQ_ID_1) ) {
        printf(" Error in [%s]: unable to complete IRQ:%d for target %d, function argument value out of range\n",__func__, irq, target);
        return false;
    }

    volatile plic_claim_reg_t *claim = (volatile plic_claim_reg_t *)PLIC_CLAIM_ADDR(target);
    volatile plic_threshold_reg_t *th = (volatile plic_threshold_reg_t *)PLIC_THRESHOLD_ADDR(target);
#ifndef PLIC_FAST_ENTRY
    unsigned long flags;
    spin_lock_irqsave(&plic_lock, flags);
#endif
    th->_b.threshold = *pSavedTH;

        /* claim->_b.id = claimedIrq;
    * Note: in above code, it wll read the claim register first then clear off the other bits and
    * then write the result back to the claim register,
    * and the read opration has spectial meaning in PLIC, So we don't need read operation here,
    * as it will acuire a IRQ and clear the penig bit.
    * */
   claim->_w = irq & PLIC_CLAIM_REG_ID_FIELD_MASK;
#ifndef PLIC_FAST_ENTRY
   spin_unlock_irqrestore(&plic_lock, flags);
#endif
   return true;
}


typedef struct {
    plic_int_target_e targets[PLIC_NUM_TARGETS];
    int count;
} target_group_t;

// Function to check shared interrupts in a specific group of targets
static inline void check_shared_interrupts(target_group_t *group) {

    char *target_name[PLIC_NUM_TARGETS]={
        "v1_m",
        "v1_s",
        "v0_m",
    };

    uint32_t common_low = 0xFFFFFFFF;
    uint32_t common_high = 0xFFFFFFFF;

    printf(" Group: {");
    for (int i = 0; i < group->count; i++) {

        volatile plic_ie_reg_t *ie_l = (volatile plic_ie_reg_t *)PLIC_IEn_L_ADDR(group->targets[i]);
        volatile plic_ie_reg_t *ie_h = (volatile plic_ie_reg_t *)PLIC_IEn_H_ADDR(group->targets[i]);
        printf(" { TARGET_%d_%s, ie_l:0x%08x le_h:0x%08x },  ",group->targets[i], target_name[group->targets[i]], ie_l->_w, ie_h->_w);
        // Update common enabled interrupts
        common_low &= ie_l->_w;
        common_high &= ie_h->_w;
    }
    printf("}\n");

    // Print shared interrupts for this group
    for (int i = 0; i < 32; i++) {
        if ((common_low >> i) & 1) {
            printf("  WARNING: Interrupt %d enabled on both target in this group!\n", i);
        }
    }
    for (int i = 32; i < 64; i++) {
        if ((common_high >> (i - 32)) & 1) {
            printf("  WARNING: Interrupt %d enabled on both target in this group!\n", i);
        }
    }
}

// Main function to check shared interrupts for all groups
void plic_check_shared_interrupts(void)
{
    target_group_t groups[]={
        { {PLIC_INT_TARGET_0_V1_M_MODE, PLIC_INT_TARGET_1_V1_S_MODE}, 2 },
        { {PLIC_INT_TARGET_0_V1_M_MODE, PLIC_INT_TARGET_2_V0_M_MODE}, 2 },
        { {PLIC_INT_TARGET_1_V1_S_MODE, PLIC_INT_TARGET_2_V0_M_MODE}, 2 },
        { {PLIC_INT_TARGET_0_V1_M_MODE, PLIC_INT_TARGET_1_V1_S_MODE, PLIC_INT_TARGET_2_V0_M_MODE}, 3 }
    };

    int num_groups = sizeof(groups) / sizeof(groups[0]);
    printf("Checking shared interrupts among multi-core...\n", num_groups);
    // Check interrupts for each generated group
    for (int i = 0; i < num_groups; i++) {
        check_shared_interrupts(&groups[i]);
    }
    printf("Checking shared interrupts end\n", num_groups);
}
