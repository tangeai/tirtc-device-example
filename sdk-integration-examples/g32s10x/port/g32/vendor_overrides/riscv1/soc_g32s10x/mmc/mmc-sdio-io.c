/*
 * Copyright (c) 2023, Ingenic Semiconductor
 *
 */
#include <stdio.h>
#include <common.h>

#include "mmc-host.h"
#include "mmc-sdio.h"
#include "mmc-sdio-io.h"

//#define MMC_DEBUG

#ifdef MMC_DEBUG
#define MMC_CORE_DBG(...)               printf("[SDIO-IO]"), printf(__VA_ARGS__)
#define MMC_CORE_WARN(...)              printf("[SDIO-IO]"), printf(__VA_ARGS__)
#define MMC_CORE_ERR(...)               printf("[SDIO-IO] Err:"), printf(__VA_ARGS__)
#else
#define MMC_CORE_DBG(...)
#define MMC_CORE_WARN(...)
#define MMC_CORE_ERR(...)               printf("[SDIO-IO] Err:"), printf(__VA_ARGS__)
#endif


#define __le16_to_cpup(x)               (*(u16 *)(x))
#define __cpu_to_le16(x)                ((u16)(x))
#define __le32_to_cpup(x)               (*(u32 *)(x))
#define __cpu_to_le32(x)                ((u32)(x))
#define le16_to_cpup                    __le16_to_cpup
#define cpu_to_le16                     __cpu_to_le16
#define le32_to_cpup                    __le32_to_cpup
#define cpu_to_le32                     __cpu_to_le32

/**
 * sdio_claim_host - exclusively claim a bus for a certain SDIO function
 * @func: SDIO function that will be accessed
 *
 * Claim a bus for a set of operations. The SDIO function given
 * is used to figure out which bus is relevant.
 */
void sdio_claim_host(struct mmc_host *host)
{
    mutex_lock(&host->mutex);
    host->claimed = 1;
}

/**
 * sdio_release_host - release a bus for a certain SDIO function
 * @func: SDIO function that was accessed
 *
 * Release a bus, allowing others to claim the bus for their
 * operations.
 */
void sdio_release_host(struct mmc_host *host)
{
    host->claimed = 0;
    mutex_unlock(&host->mutex);
}


/**
 * sdio_enable_func  enables a SDIO function for usage
 * @func: SDIO function to enable
 *
 * Powers up and activates a SDIO function so that register
 * access is possible.
 */
int sdio_enable_func(struct sdio_func *func)
{
    int ret;
    unsigned char reg;
    unsigned long timeout_ms;

    assert(func);
    assert(func->card);

    MMC_CORE_DBG("SDIO: Enabling device...\n");

    ret = sdio_io_rw_direct(func->card, 0, 0, SDIO_CCCR_IOEx, 0, &reg);
    if (ret)
        goto err;

    reg |= 1 << func->num;

    ret = sdio_io_rw_direct(func->card, 1, 0, SDIO_CCCR_IOEx, reg, NULL);
    if (ret)
        goto err;

    timeout_ms = systick_get_time_ms() + func->enable_timeout;

    while (1) {
        ret = sdio_io_rw_direct(func->card, 0, 0, SDIO_CCCR_IORx, 0, &reg);
        if (ret)
            goto err;
        if (reg & (1 << func->num))
            break;
        ret = -ETIME;
        if (systick_get_time_ms() > timeout_ms)
            goto err;
    }

    MMC_CORE_DBG("SDIO: Enabled device\n");

    return 0;

err:
    printf("SDIO: Failed to enable device\n");
    return ret;
}


/**
 * sdio_disable_func  disable a SDIO function
 * @func: SDIO function to disable
 *
 * Powers down and deactivates a SDIO function. Register access
 * to this function will fail until the function is reenabled.
 */
int sdio_disable_func(struct sdio_func *func)
{
    int ret;
    unsigned char reg;

    assert(func);
    assert(func->card);

    MMC_CORE_DBG("SDIO: Disabling device...\n");

    ret = sdio_io_rw_direct(func->card, 0, 0, SDIO_CCCR_IOEx, 0, &reg);
    if (ret)
        goto err;

    reg &= ~(1 << func->num);

    ret = sdio_io_rw_direct(func->card, 1, 0, SDIO_CCCR_IOEx, reg, NULL);
    if (ret)
        goto err;

    MMC_CORE_DBG("SDIO: Disabled device\n");

    return 0;

err:
    printf("SDIO: Failed to disable device\n");
    return -EIO;
}

/**
 * sdio_set_block_size - set the block size of an SDIO function
 * @func: SDIO function to change
 * @blksz: new block size or 0 to use the default.
 *
 * The default block size is the largest supported by both the function
 * and the host, with a maximum of 512 to ensure that arbitrarily sized
 * data transfer use the optimal (least) number of commands.
 *
 * A driver may call this to override the default block size set by the
 * core. This can be used to set a block size greater than the maximum
 * that reported by the card; it is the driver's responsibility to ensure
 * it uses a value that the card supports.
 *
 * Returns 0 on success, -EINVAL if the host does not support the
 * requested block size, or -EIO (etc.) if one of the resultant FBR block
 * size register writes failed.
 *
 */
int sdio_set_block_size(struct sdio_func *func, unsigned blksz)
{
    int ret;

    if (blksz > func->card->host->max_blk_size)
        return -EINVAL;

    if (blksz == 0) {
        blksz = min(func->max_blksize, func->card->host->max_blk_size);
        blksz = min(blksz, 512u);
    }

    ret = sdio_io_rw_direct(func->card, 1, 0,
        SDIO_FBR_BASE(func->num) + SDIO_FBR_BLKSIZE,
        blksz & 0xff, NULL);
    if (ret)
        return ret;

    ret = sdio_io_rw_direct(func->card, 1, 0,
        SDIO_FBR_BASE(func->num) + SDIO_FBR_BLKSIZE + 1,
        (blksz >> 8) & 0xff, NULL);
    if (ret)
        return ret;

    func->cur_blksize = blksz;

    return 0;
}

/*
 * Calculate the maximum byte mode transfer size
 */
static inline unsigned int sdio_max_byte_size(struct sdio_func *func)
{
    unsigned int mval = func->card->host->max_blk_size;

    if (mmc_blksz_for_byte_mode(func->card))
        mval = min(mval, (unsigned int)func->cur_blksize);
    else
        mval = min(mval, (unsigned int)func->max_blksize);

    if (mmc_card_broken_byte_mode_512(func->card))
        return min(mval, 511u);

    return min(mval, 512u); /* maximum size for byte mode */
}


/*
 * Split an arbitrarily sized data transfer into several
 * IO_RW_EXTENDED commands.
 */
static int sdio_io_rw_ext_helper(struct sdio_func *func, int write,
    unsigned int addr, int incr_addr, uint8_t *buf, unsigned int size)
{
    unsigned int remainder = size;
    unsigned int max_blocks;
    unsigned int blocks;
    int ret;

    /* Do the bulk of the transfer using block mode (if supported). */
    if (func->card->cccr.multi_block && (size > sdio_max_byte_size(func))) {
        /* Blocks per command is limited by host count, host transfer
         * size and the maximum for IO_RW_EXTENDED of 511 blocks. */
        max_blocks = min(func->card->host->max_blk_count, (uint32_t)511U);

        while (remainder >= func->cur_blksize) {

            blocks = remainder / (func->cur_blksize);
            if (blocks > max_blocks)
                blocks = max_blocks;
            size = blocks * (func->cur_blksize);

            ret = sdio_io_rw_extended(func->card, write,
                func->num, addr, incr_addr, buf,
                blocks, func->cur_blksize);
            if (ret)
                return ret;

            remainder -= size;
            buf += size;
            if (incr_addr)
                addr += size;
        }
    }

    /* Write the remainder using byte mode. */
    while (remainder > 0) {
        size = min(remainder, sdio_max_byte_size(func));

        /* Indicate byte mode by setting "blocks" = 0 */
        ret = sdio_io_rw_extended(func->card, write, func->num, addr, incr_addr, buf, 0, size);
        if (ret)
            return ret;

        remainder -= size;
        buf += size;
        if (incr_addr)
            addr += size;
    }

    return 0;
}

/**
 * sdio_readb - read a single byte from a SDIO function
 * @func: SDIO function to access
 * @addr: address to read
 * @err_ret: optional status value from transfer
 *
 * Reads a single byte from the address space of a given SDIO
 * function. If there is a problem reading the address, 0xff
 * is returned and @err_ret will contain the error code.
 */
uint8_t sdio_readb(struct sdio_func *func, unsigned int addr, int *err_ret)
{
    int ret;
    uint8_t val;

    assert(func);

    if (err_ret)
        *err_ret = 0;

    ret = sdio_io_rw_direct(func->card, 0, func->num, addr, 0, &val);
    if (ret) {
        if (err_ret)
            *err_ret = ret;
        return 0xFF;
    }

    return val;
}

/**
 * sdio_writeb - write a single byte to a SDIO function
 * @func: SDIO function to access
 * @b: byte to write
 * @addr: address to write to
 * @err_ret: optional status value from transfer
 *
 * Writes a single byte to the address space of a given SDIO
 * function. @err_ret will contain the status of the actual
 * transfer.
 */
void sdio_writeb(struct sdio_func *func, uint8_t b, unsigned int addr, int *err_ret)
{
    int ret;

    assert(func);

    ret = sdio_io_rw_direct(func->card, 1, func->num, addr, b, NULL);
    if (err_ret)
        *err_ret = ret;
}

/**
 * sdio_writeb_readb - write and read a byte from SDIO function
 * @func: SDIO function to access
 * @write_byte: byte to write
 * @addr: address to write to
 * @err_ret: optional status value from transfer
 *
 * Performs a RAW (Read after Write) operation as defined by SDIO spec -
 * single byte is written to address space of a given SDIO function and
 * response is read back from the same address, both using single request.
 * If there is a problem with the operation, 0xff is returned and
 * @err_ret will contain the error code.
 */
uint8_t sdio_writeb_readb(struct sdio_func *func, uint8_t write_byte,
    unsigned int addr, int *err_ret)
{
    int ret;
    uint8_t val;

    ret = sdio_io_rw_direct(func->card, 1, func->num, addr,
            write_byte, &val);
    if (err_ret)
        *err_ret = ret;
    if (ret)
        val = 0xff;

    return val;
}

/**
 * sdio_memcpy_fromio - read a chunk of memory from a SDIO function
 * @func: SDIO function to access
 * @dst: buffer to store the data
 * @addr: address to begin reading from
 * @count: number of bytes to read
 *
 * Reads from the address space of a given SDIO function. Return
 * value indicates if the transfer succeeded or not.
 */
int sdio_memcpy_fromio(struct sdio_func *func, void *dst,
    unsigned int addr, int count)
{
    return sdio_io_rw_ext_helper(func, 0, addr, 1, dst, count);
}

/**
 * sdio_memcpy_toio - write a chunk of memory to a SDIO function
 * @func: SDIO function to access
 * @addr: address to start writing to
 * @src: buffer that contains the data to write
 * @count: number of bytes to write
 *
 * Writes to the address space of a given SDIO function. Return
 * value indicates if the transfer succeeded or not.
 */
int sdio_memcpy_toio(struct sdio_func *func, unsigned int addr,
    void *src, int count)
{
    return sdio_io_rw_ext_helper(func, 1, addr, 1, src, count);
}

/**
 * sdio_readsb - read from a FIFO on a SDIO function
 * @func: SDIO function to access
 * @dst: buffer to store the data
 * @addr: address of (single byte) FIFO
 * @count: number of bytes to read
 *
 * Reads from the specified FIFO of a given SDIO function. Return
 * value indicates if the transfer succeeded or not.
 */
int sdio_readsb(struct sdio_func *func, void *dst, unsigned int addr,
    int count)
{
    return sdio_io_rw_ext_helper(func, 0, addr, 0, dst, count);
}

/**
 * sdio_writesb - write to a FIFO of a SDIO function
 * @func: SDIO function to access
 * @addr: address of (single byte) FIFO
 * @src: buffer that contains the data to write
 * @count: number of bytes to write
 *
 * Writes to the specified FIFO of a given SDIO function. Return
 * value indicates if the transfer succeeded or not.
 */
int sdio_writesb(struct sdio_func *func, unsigned int addr, void *src,
    int count)
{
    return sdio_io_rw_ext_helper(func, 1, addr, 0, src, count);
}

/**
 * sdio_readw - read a 16 bit integer from a SDIO function
 * @func: SDIO function to access
 * @addr: address to read
 * @err_ret: optional status value from transfer
 *
 * Reads a 16 bit integer from the address space of a given SDIO
 * function. If there is a problem reading the address, 0xffff
 * is returned and @err_ret will contain the error code.
 */
uint16_t sdio_readw(struct sdio_func *func, unsigned int addr, int *err_ret)
{
    int ret;

    if (err_ret)
        *err_ret = 0;

    ret = sdio_memcpy_fromio(func, func->tmpbuf, addr, 2);
    if (ret) {
        if (err_ret)
            *err_ret = ret;
        return 0xFFFF;
    }

    return le16_to_cpup((uint16_t *)func->tmpbuf);
}

/**
 * sdio_writew - write a 16 bit integer to a SDIO function
 * @func: SDIO function to access
 * @b: integer to write
 * @addr: address to write to
 * @err_ret: optional status value from transfer
 *
 * Writes a 16 bit integer to the address space of a given SDIO
 * function. @err_ret will contain the status of the actual
 * transfer.
 */
void sdio_writew(struct sdio_func *func, uint16_t b, unsigned int addr, int *err_ret)
{
    int ret;

    *(uint16_t *)func->tmpbuf = cpu_to_le16(b);

    ret = sdio_memcpy_toio(func, addr, func->tmpbuf, 2);
    if (err_ret)
        *err_ret = ret;
}

/**
 * sdio_readl - read a 32 bit integer from a SDIO function
 * @func: SDIO function to access
 * @addr: address to read
 * @err_ret: optional status value from transfer
 *
 * Reads a 32 bit integer from the address space of a given SDIO
 * function. If there is a problem reading the address,
 * 0xffffffff is returned and @err_ret will contain the error
 * code.
 */
uint32_t sdio_readl(struct sdio_func *func, unsigned int addr, int *err_ret)
{
    int ret;

    if (err_ret)
        *err_ret = 0;

    ret = sdio_memcpy_fromio(func, func->tmpbuf, addr, 4);
    if (ret) {
        if (err_ret)
            *err_ret = ret;
        return 0xFFFFFFFF;
    }

    return le32_to_cpup((u32 *)func->tmpbuf);
}

/**
 * sdio_writel - write a 32 bit integer to a SDIO function
 * @func: SDIO function to access
 * @b: integer to write
 * @addr: address to write to
 * @err_ret: optional status value from transfer
 *
 * Writes a 32 bit integer to the address space of a given SDIO
 * function. @err_ret will contain the status of the actual
 * transfer.
 */
void sdio_writel(struct sdio_func *func, uint32_t b, unsigned int addr, int *err_ret)
{
    int ret;

    *(uint32_t *)func->tmpbuf = cpu_to_le32(b);

    ret = sdio_memcpy_toio(func, addr, func->tmpbuf, 4);
    if (err_ret)
        *err_ret = ret;
}

/**
 * sdio_f0_readb - read a single byte from SDIO function 0
 * @func: an SDIO function of the card
 * @addr: address to read
 * @err_ret: optional status value from transfer
 *
 * Reads a single byte from the address space of SDIO function 0.
 * If there is a problem reading the address, 0xff is returned
 * and @err_ret will contain the error code.
 */
unsigned char sdio_f0_readb(struct sdio_func *func, unsigned int addr,
    int *err_ret)
{
    int ret;
    unsigned char val;

    assert(func);

    if (err_ret)
        *err_ret = 0;

    ret = sdio_io_rw_direct(func->card, 0, 0, addr, 0, &val);
    if (ret) {
        if (err_ret)
            *err_ret = ret;
        return 0xFF;
    }

    return val;
}

/**
 * sdio_f0_writeb - write a single byte to SDIO function 0
 * @func: an SDIO function of the card
 * @b: byte to write
 * @addr: address to write to
 * @err_ret: optional status value from transfer
 *
 * Writes a single byte to the address space of SDIO function 0.
 * @err_ret will contain the status of the actual transfer.
 *
 * Only writes to the vendor specific CCCR registers (0xF0 -
 * 0xFF) are permiited; @err_ret will be set to -EINVAL for *
 * writes outside this range.
 */
void sdio_f0_writeb(struct sdio_func *func, unsigned char b, unsigned int addr,
    int *err_ret)
{
    int ret;

    assert(func);

    if ((addr < 0xF0 || addr > 0xFF) && (!mmc_card_lenient_fn0(func->card))) {
        if (err_ret)
            *err_ret = -EINVAL;
        return;
    }

    ret = sdio_io_rw_direct(func->card, 1, 0, addr, b, NULL);
    if (err_ret)
        *err_ret = ret;
}

#include <kernel_symbol.h>

EXPORT_SYMBOL(sdio_claim_host);
EXPORT_SYMBOL(sdio_release_host);
EXPORT_SYMBOL(sdio_enable_func);
EXPORT_SYMBOL(sdio_disable_func);
EXPORT_SYMBOL(sdio_set_block_size);
EXPORT_SYMBOL(sdio_readb);
EXPORT_SYMBOL(sdio_writeb);
EXPORT_SYMBOL(sdio_writeb_readb);
EXPORT_SYMBOL(sdio_memcpy_fromio);
EXPORT_SYMBOL(sdio_memcpy_toio);
EXPORT_SYMBOL(sdio_readsb);
EXPORT_SYMBOL(sdio_writesb);
EXPORT_SYMBOL(sdio_readw);
EXPORT_SYMBOL(sdio_writew);
EXPORT_SYMBOL(sdio_readl);
EXPORT_SYMBOL(sdio_writel);
EXPORT_SYMBOL(sdio_f0_readb);
EXPORT_SYMBOL(sdio_f0_writeb);
