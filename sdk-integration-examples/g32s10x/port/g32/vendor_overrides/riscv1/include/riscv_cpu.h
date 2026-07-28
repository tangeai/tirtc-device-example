
#ifndef _RISCV_CPU_H_
#define _RISCV_CPU_H_

#include <stdint.h>

#define RISCV_CPU_LOCAL_INTERRUPT_SW 8 /* Bit3 0x008 */
#define RISCV_CPU_LOCAL_INTERRUPT_TMR 128 /* Bit7 0x080 */
#define RISCV_CPU_LOCAL_INTERRUPT_EXT 2048 /* Bit11 0x800 */



typedef enum {
    RISCV_CPU_INTERRUPT_ID_BASE,
    RISCV_CPU_INTERRUPT_ID_SW = (RISCV_CPU_INTERRUPT_ID_BASE + 3),
    RISCV_CPU_INTERRUPT_ID_TMR = (RISCV_CPU_INTERRUPT_ID_BASE + 7),
    RISCV_CPU_INTERRUPT_ID_EXT = (RISCV_CPU_INTERRUPT_ID_BASE + 11)
}riscv_cpu_interrupt_id_e;

uintptr_t riscv_cpu_myhart_id(void);
void riscv_cpu_interrupt_global_enable(void);
void riscv_cpu_interrupt_global_disable(void);
uint32_t riscv_cpu_save_and_disable_global_int(void);
void riscv_cpu_restore_global_interrupts(uint32_t saved_mstatus_mie);
void riscv_cpu_interrupt_software_enable(void);
void riscv_cpu_interrupt_software_disable(void);
void riscv_cpu_interrupt_mtimer_enable(void);
void riscv_cpu_interrupt_mtimer_disable(void);
void riscv_cpu_interrupt_external_enable(void);
void riscv_cpu_interrupt_external_disable(void);

void riscv_cpu_interrupt_local_enable(riscv_cpu_interrupt_id_e id);
void riscv_cpu_interrupt_local_disable(riscv_cpu_interrupt_id_e id);



void print_mstatus(void);
void riscv_cpu_interrupt_regs_info(void);

void riscv_cpu_reg_info_fcsr(uint32_t fcsr);
void riscv_cpu_reg_info_floating_point(uint32_t *fp_registers_ptr);


// Define the mstatus structure
typedef union {
    struct {
        uint32_t uie : 1;    // User Interrupt Enable (Bit 0)
        uint32_t sie : 1;    // Supervisor Interrupt Enable (Bit 1)
        uint32_t reserved1 : 1;
        uint32_t mie : 1;    // Machine Interrupt Enable (Bit 3)
        uint32_t reserved2 : 1;
        uint32_t spie : 1;   // Supervisor Previous Interrupt Enable (Bit 5)
        uint32_t reserved3 : 1;
        uint32_t mpie : 1;   // Machine Previous Interrupt Enable (Bit 7)
        uint32_t spp : 1;    // Supervisor Previous Privilege Mode (Bit 8)
        uint32_t reserved4 : 2;
        uint32_t mpp : 2;    // Machine Previous Privilege Mode (Bits 12:11)
        uint32_t fs : 2;     // Floating-Point Status (Bits 14:13)
        uint32_t xs : 2;     // Extension Status (Bits 16:15)
        uint32_t mprv : 1;   // Modify Privilege (Bit 17)
        uint32_t sum : 1;    // Supervisor User Memory Access (Bit 18)
        uint32_t mxr : 1;    // Make Executable Readable (Bit 19)
        uint32_t tvm : 1;    // Trap Virtual Memory (Bit 20)
        uint32_t tw : 1;     // Timeout Wait (Bit 21)
        uint32_t tsr : 1;    // Trap SRET (Bit 22)
        uint32_t reserved5 : 8;
        uint32_t sd : 1;     // State Dirty (Bit 31)
    } bits;                  // Access individual bit fields
    uint32_t value;          // Access the entire register as a 32-bit value
} mstatus_t;

// Enum definition for mstatus.fs field states
typedef enum {
    MSTATUS_FS_E_OFF = 0,       // Off: FPU is disabled
    MSTATUS_FS_E_INITIAL = 1,   // Initial: FPU is initialized but unused
    MSTATUS_FS_E_CLEAN = 2,     // Clean: FPU state is consistent
    MSTATUS_FS_E_DIRTY = 3      // Dirty: FPU has been modified
} mstatus_fs_e;
// Define the structure for the FCSR register
typedef struct {
    uint32_t fflags : 5;   // Accrued exception flags (bits 0–4)
    uint32_t frm : 3;      // Rounding mode (bits 5–7)
    uint32_t reserved : 24; // Reserved bits (bits 8–31)
} fcsr_t;

// Floating-point exception flags for FCSR
#define FCSR_EXC_FFLAG_INVALID_OPERATION 0x01
#define FCSR_EXC_FFLAG_DIVIDE_BY_ZERO    0x02
#define FCSR_EXC_FFLAG_OVERFLOW          0x04
#define FCSR_EXC_FFLAG_UNDERFLOW         0x08
#define FCSR_EXC_FFLAG_INEXACT_RESULT    0x10

#define CPU_NAME "riscv"

#endif
