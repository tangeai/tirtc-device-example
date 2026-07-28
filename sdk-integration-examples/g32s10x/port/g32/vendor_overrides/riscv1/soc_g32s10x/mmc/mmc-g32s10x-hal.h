/*
 * Copyright (c) 2021, Ingenic Semiconductor
 *
 */

#ifndef __MMC_G32S10X_HAL_H__
#define __MMC_G32S10X_HAL_H__

#include <common.h>

static const unsigned long msc_iobase[] = {
    (unsigned long)io_addr(MSC0_IOBASE),
    (unsigned long)io_addr(MSC1_IOBASE),
};

#define MSC_ADDR8(id, reg)              ((volatile unsigned char  *)((msc_iobase[id]) + (reg)))
#define MSC_ADDR16(id, reg)             ((volatile unsigned short *)((msc_iobase[id]) + (reg)))
#define MSC_ADDR32(id, reg)             ((volatile unsigned int   *)((msc_iobase[id]) + (reg)))

/*
 * bit operation
 */
static inline void msc_write_reg_8(int id, unsigned int reg, unsigned char value)
{
    *MSC_ADDR8(id, reg) = value;
}

static inline void msc_write_reg_16(int id, unsigned int reg, unsigned short value)
{
    *MSC_ADDR16(id, reg) = value;
}

static inline void msc_write_reg_32(int id, unsigned int reg, unsigned int value)
{
    *MSC_ADDR32(id, reg) = value;
}

static inline unsigned char msc_read_reg_8(int id, unsigned int reg)
{
    return *MSC_ADDR8(id, reg);
}

static inline unsigned short msc_read_reg_16(int id, unsigned int reg)
{
    return *MSC_ADDR16(id, reg);
}

static inline unsigned int msc_read_reg_32(int id, unsigned int reg)
{
    return *MSC_ADDR32(id, reg);
}

static inline void msc_set_bits_8(int id, unsigned int reg, int start, int end, unsigned char val)
{
    volatile unsigned char *reg_off = MSC_ADDR8(id, reg);
    unsigned char mask = bit_field_mask(start, end);

    *reg_off = (*reg_off & ~mask) | ((val << start) & mask);
}

static inline void msc_set_bits_16(int id, unsigned int reg, int start, int end, unsigned short val)
{
    volatile unsigned short *reg_off = MSC_ADDR16(id, reg);
    unsigned short mask = bit_field_mask(start, end);

    *reg_off = (*reg_off & ~mask) | ((val << start) & mask);
}

static inline void msc_set_bits_32(int id, unsigned int reg, int start, int end, unsigned int val)
{
    volatile unsigned int *reg_off = MSC_ADDR32(id, reg);
    unsigned int mask = bit_field_mask(start, end);

    *reg_off = (*reg_off & ~mask) | ((val << start) & mask);
}

static inline unsigned char msc_get_bits_8(int id, unsigned int reg, int start, int end)
{
    volatile unsigned char *reg_off = MSC_ADDR8(id, reg);

    return (*reg_off & bit_field_mask(start, end)) >> start;
}

static inline unsigned short msc_get_bits_16(int id, unsigned int reg, int start, int end)
{
    volatile unsigned short *reg_off = MSC_ADDR16(id, reg);

    return (*reg_off & bit_field_mask(start, end)) >> start;
}

static inline unsigned int msc_get_bits_32(int id, unsigned int reg, int start, int end)
{
    volatile unsigned int *reg_off = MSC_ADDR32(id, reg);

    return (*reg_off & bit_field_mask(start, end)) >> start;
}


extern void mmc_hal_set_sdma_address(int index, uint32_t addr);
extern void mmc_hal_set_block_size(int index, uint16_t value);
extern void mmc_hal_set_sdma_buffer_boundary(int index, uint16_t value);
extern void mmc_hal_set_block_count(int index, uint16_t value);
extern void mmc_hal_set_argument(int index, uint32_t value);
extern uint16_t mmc_hal_get_transfer_mode(int index);
extern void mmc_hal_set_transfer_mode(int index, uint16_t mode);
extern void mmc_hal_set_data_transfer_direction_read(int index);
extern void mmc_hal_set_command(int index, uint16_t value);
extern uint32_t mmc_hal_read_bufferdata(int index);
extern void mmc_hal_write_bufferdata(int index, uint32_t value);
extern int mmc_hal_get_presend_status(int index);
extern void mmc_hal_set_transfer_width_enable_4bit(int index, int enable);
extern void mmc_hal_set_transfer_width_enable_8bit(int index, int enable);
extern void mmc_hal_set_dma_mode(int index, msc_dma_mode mode);
extern void mmc_hal_set_high_speed_enable(int index, int enable);
extern int mmc_hal_get_high_speed_enable(int index);
extern void mmc_hal_set_power_control(int index, uint8_t value);
extern void mmc_hal_block_gap_request_stop(int index);
extern void mmc_hal_block_gap_request_start(int index);
extern void mmc_hal_clock_control_enable(int index, uint32_t enable);
extern int mmc_hal_clock_control_initialization_stable(int index);
extern void mmc_hal_clock_control_initialization_enable(int index, uint16_t enable);
extern void mmc_hal_set_data_timeout(int index, uint8_t value);
extern void mmc_hal_software_reset_controller(int index);
extern void mmc_hal_software_reset_cmd_data(int index);
extern int mmc_hal_software_is_resetting_controller(int index);
extern int mmc_hal_software_is_resetting_cmd_data(int index);
extern void mmc_hal_clear_int_flag_transfer_complete(int index);
extern int mmc_hal_is_transfer_complete(int index);
extern void mmc_hal_clear_int_flag_response_complete(int index);
extern int mmc_hal_is_response_complete(int index);
extern int mmc_hal_get_normal_int_status(int index);
extern void mmc_hal_clear_normal_int_status(int index, uint16_t value);
extern int mmc_hal_get_error_int_status(int index);
extern void mmc_hal_clear_error_int_status(int index, uint16_t value);
extern uint32_t mmc_hal_get_all_int_status(int index);
extern void mmc_hal_clear_all_int_status(int index, uint32_t value);
extern void mmc_hal_enable_normal_interrupt(int index, uint16_t value);
extern uint16_t mmc_hal_get_normal_interrupt_status(int index);
extern void mmc_hal_enable_error_interrupt(int index, uint16_t value);
extern uint16_t mmc_hal_get_error_interrupt_status(int index);
extern void mmc_hal_enable_normal_interrupt_signal(int index, uint16_t value);
extern uint16_t mmc_hal_get_normal_interrupt_signal_status(int index);
extern void mmc_hal_enable_error_interrupt_signal(int index, uint16_t value);
extern uint16_t mmc_hal_get_error_interrupt_signal_status(int index);
extern int mmc_hal_get_tuned_clk(int index);
extern void mmc_hal_enable_tuned_clk(int index, int value);
extern int mmc_hal_get_exec_tuning(int index);
extern void mmc_hal_set_exec_tuning(int index);
extern void mmc_hal_set_uhs_signaling(int index, int value);
extern int mmc_hal_get_uhs_signaling(int index);
extern uint32_t mmc_hal_get_host_capablites_max_block_length(int index);
extern int mmc_hal_get_max_current_330(int index);
extern int mmc_hal_get_max_current_300(int index);
extern int mmc_hal_get_max_current_180(int index);
extern int mmc_hal_get_host_vendor_version(int index);
extern int mmc_hal_get_host_sepc_version(int index);

#endif /* __MMC_G32S10X_HAL_H__ */
