/*
 * Copyright (c) 2021, Ingenic Semiconductor
 *
 */
#include <stdio.h>
#include <sizes.h>
#include <common.h>
#include "mmc-core.h"
#include "mmc-host.h"
#include "mmc-block.h"
#include "mmc-sd.h"

//#define DEBUG_DEVICE_INFO
//#define DUPM_DEVICE_INFO
//#define MMC_SD_DEBUG

#ifdef MMC_SD_DEBUG
#define MMC_SD_DBG(...)                 printf("[SD] Debug:"), printf(__VA_ARGS__)
#define MMC_SD_WARN(...)                printf("[SD] Warn:"), printf(__VA_ARGS__)
#define MMC_SD_ERR(...)                 printf("[SD] Err:"), printf(__VA_ARGS__)
#else
#define MMC_SD_DBG(...)
#define MMC_SD_WARN(...)
#define MMC_SD_ERR(...)                 printf("[MMC] Err:"), printf(__VA_ARGS__)
#endif

int mmc_send_cmd_data(struct mmc_host *mmc, struct mmc_cmd* cmd, struct mmc_data* data);
int mmc_all_get_cid(struct mmc_host *mmc, uint32_t *cid);
int mmc_select_card(struct mmc_card *card);
int mmc_get_csd(struct mmc_card *card, uint32_t *csd);

static const unsigned int tran_exp[] = {
    10000,      100000,     1000000,    10000000,
    0,          0,          0,          0
};

static const unsigned char tran_mant[] = {
    0,  10, 12, 13, 15, 20, 25, 30,
    35, 40, 45, 50, 55, 60, 70, 80,
};

static const unsigned int tacc_exp[] = {
    1,  10, 100,    1000,   10000,  100000, 1000000,    10000000,
};

static const unsigned int tacc_mant[] = {
    0,  10, 12, 13, 15, 20, 25, 30,
    35, 40, 45, 50, 55, 60, 70, 80,
};

static const unsigned int sd_au_size[] = {
    0,              SZ_16K / 512,           SZ_32K / 512,   SZ_64K / 512,
    SZ_128K / 512,  SZ_256K / 512,          SZ_512K / 512,  SZ_1M / 512,
    SZ_2M / 512,    SZ_4M / 512,            SZ_8M / 512,    (SZ_8M + SZ_4M) / 512,
    SZ_16M / 512,   (SZ_16M + SZ_8M) / 512, SZ_32M / 512,   SZ_64M / 512,
};

#define UNSTUFF_BITS(resp, start, size)                         \
    ({                                                          \
        const int __size = size;                                \
        const u32 __mask = (__size < 32 ? 1 << __size : 0) - 1; \
        const int __off = 3 - ((start) / 32);                   \
        const int __shft = (start) & 31;                        \
        u32 __res;                                              \
                                                                \
        __res = resp[__off] >> __shft;                          \
        if (__size + __shft > 32)                               \
            __res |= resp[__off-1] << ((32 - __shft) % 32);     \
        __res & __mask;                                         \
    })

#define ___constant_swab32(x)         ((uint32_t)(              \
    (((uint32_t)(x) & (uint32_t)0x000000ffUL) << 24) |          \
    (((uint32_t)(x) & (uint32_t)0x0000ff00UL) <<  8) |          \
    (((uint32_t)(x) & (uint32_t)0x00ff0000UL) >>  8) |          \
    (((uint32_t)(x) & (uint32_t)0xff000000UL) >> 24)))

static inline uint32_t be32_to_cpu(uint32_t val)
{
    return ___constant_swab32(val);
}

static uint32_t sd_get_host_max_current(struct mmc_host *mmc)
{
    uint32_t voltage = 1 << mmc->min_voltage;
    uint32_t max_current;

    switch (voltage) {
    case MMC_VDD_165_195:
        max_current = mmc->max_current_180;
        break;
    case MMC_VDD_29_30:
    case MMC_VDD_30_31:
        max_current = mmc->max_current_300;
        break;
    case MMC_VDD_32_33:
    case MMC_VDD_33_34:
        max_current = mmc->max_current_330;
        break;
    default:
        max_current = 0;
        break;
    }

    return max_current;
}

static int mmc_app_cmd(struct mmc_host *mmc, struct mmc_card *card)
{
    int err;
    struct mmc_cmd cmd = {0};

    cmd.opcode = MMC_APP_CMD;
    cmd.retries = 0;

    if (card) {
        cmd.arg = card->rca << 16;
        cmd.resp_type = MMC_RSP_R1 | MMC_CMD_AC;
    } else {
        cmd.arg = 0;
        cmd.resp_type =  MMC_RSP_R1 | MMC_CMD_BCR;
    }

    err = mmc_send_cmd_data(mmc, &cmd, NULL);
    if (err)
        return err;

    /* Check that card supported application commands */
    if (!(cmd.resp[0] & R1_APP_CMD))
        return -EOPNOTSUPP;

    return 0;
}

static int mmc_send_app_cmd_data(struct mmc_host *mmc,
        struct mmc_card *card, struct mmc_cmd* cmd, struct mmc_data* data, int retries)
{
    int ret = 0;
    int i = 0;

    /*
     * 先发送APP_CMD,设备在接收到APP_CMD之后，Card设备设置APP_CMD标志，使用non standard version解析后续的命令
     * 当设备接收到non-ACMD命令后，Card设备清除APP_CMD标志.
     * 不使用retries重试单个CMD,需要按照设备接受的格式APP_CMD + CMD重试
     */
    for (i = 0; i <= retries; i++) {
        ret = mmc_app_cmd(mmc, card);
        if (ret)
            continue;

        cmd->retries = 0;
        ret = mmc_send_cmd_data(mmc, cmd, data);
        if (!ret)
            break;
    }

    return ret;
}

/*
 * SD operations
 */
static int mmc_send_app_op_cond(struct mmc_host *mmc, uint32_t ocr, uint32_t *rocr)
{
    struct mmc_cmd cmd = {0};
    int ret = 0;
    int i = 0;

    cmd.opcode = SD_APP_OP_COND;
    cmd.arg = ocr;
    cmd.resp_type = MMC_RSP_R3 | MMC_CMD_BCR;
    cmd.retries = 3;

    for (i=0; i<100; i++) {
        ret = mmc_send_app_cmd_data(mmc, NULL, &cmd, NULL, cmd.retries);
        if (ret)
            break;

        /* if we're just probing, do a single pass */
        if (ocr == 0)
            break;

        /* wait until reset completes */
        if (cmd.resp[0] & MMC_CARD_BUSY)
            break;

        ret = -ETIMEDOUT;
        mdelay(10);
    }

    if (rocr)
        *rocr = cmd.resp[0];

    return ret;
}

static int mmc_send_if_cond(struct mmc_host *mmc, uint32_t ocr)
{
    struct mmc_cmd cmd = {0};
    int ret;
    const uint8_t test_pattern = 0xAA;
    uint8_t result_pattern;

    /*
     * To support SD 2.0 cards, we must always invoke SD_SEND_IF_COND
     * before SD_APP_OP_COND. This command will harmlessly fail for
     * SD 1.0 cards.
     */
    cmd.opcode      = SD_SEND_IF_COND;
    cmd.arg         = ((ocr & 0xFF8000) != 0) << 8 | test_pattern;
    cmd.resp_type   = MMC_RSP_R7;

    ret = mmc_send_cmd_data(mmc, &cmd, NULL);
    if (ret)
        return ret;

    result_pattern = cmd.resp[0] & 0xFF;

    if (result_pattern != test_pattern)
        return -EIO;

    return 0;
}

static int mmc_get_card_relative_addr(struct mmc_host *mmc, uint32_t *rca)
{
    struct mmc_cmd cmd = {0};
    int ret;

    cmd.opcode      = SD_SEND_RELATIVE_ADDR;
    cmd.arg         = 0;
    cmd.resp_type   = MMC_RSP_R6 | MMC_CMD_BCR;

    ret = mmc_send_cmd_data(mmc, &cmd, NULL);
    if (ret)
        return ret;

    *rca = cmd.resp[0] >> 16;

    return 0;
}

static int __mmc_debug_dump_device_info(struct mmc_card *card)
{
#ifdef DEBUG_DEVICE_INFO
    struct mmc_csd *csd = &card->csd;
    struct mmc_cid *cid = &card->cid;

    /*
     * Dump CSD
     */
#ifdef DUPM_DEVICE_INFO
    printf("=================Device CSD Structure===========\n");
    printf("csd_resp[0]             : 0x%x\n",      card->raw_csd[0]);
    printf("csd_resp[1]             : 0x%x\n",      card->raw_csd[1]);
    printf("csd_resp[2]             : 0x%x\n",      card->raw_csd[2]);
    printf("csd_resp[3]             : 0x%x\n",      card->raw_csd[3]);

    printf("CSD structure version   : %d\n",        csd->structure);
    printf("TAAC                    : 0x%x\n",      csd->taac);
    printf("NSAC                    : 0x%x\n",      csd->nsac);
    printf("Tran Speed              : %d\n",        csd->tran_speed);
    printf("C_SIZE                  : %d\n",        csd->c_size);
    printf("C_SIZE_MULT             : %d\n",        csd->c_size_mult);
    printf("Read to Write Factor    : %d\n",        csd->r2w_factor);

    printf("Card TACC Clks          : %d\n",        card->tacc_clks);
    printf("Card TACC ns            : %d\n",        card->tacc_ns);
    printf("\n");
#endif

    printf("=================Device Information===========\n");
    /*
     * Dump CID
     */
    printf("ManufacturerID          : 0x%X\n", cid->manfid);
    printf("OEMID                   : 0x%X\n", cid->oemid);
    printf("ProductName             : %c%c%c%c%c%c\n", cid->prod_name[0],
                                                       cid->prod_name[1],
                                                       cid->prod_name[2],
                                                       cid->prod_name[3],
                                                       cid->prod_name[4],
                                                       cid->prod_name[5]);

    printf("SD version              : %d\n",        csd->mmca_vsn);
    printf("Device Command Class    : 0x%x\n",      csd->cmd_class);
    printf("Read Block Lenght       : %d Byte\n",   csd->rd_blk_len);
    printf("Write Block Lenght      : %d Byte\n",   csd->wr_blk_len);
    printf("Card Erase Group Size   : %d Bytes\n",  card->erase_size);
    printf("Max Freq                : %d MHz\n",    card->max_data_rate / 1000 / 1000);
    printf("Device Capacity         : %lld KBytes\n", card->card_capacity);
#endif

    return 0;
}

static void __mmc_debug_dump_device_configure(struct mmc_card *card)
{
#ifdef MMC_SD_DEBUG
    uint32_t max_rate = card->sw_caps.hs_max_dtr;
    uint32_t bus_mode = card->sw_caps.sd3_bus_mode;
    char *bus_mode_str;
    if (bus_mode & SD_MODE_HIGH_SPEED)
        bus_mode_str = "High-Speed";
    else if (bus_mode & SD_MODE_UHS_DDR50)
        bus_mode_str = "UHS-DDR50";
    else if (bus_mode & SD_MODE_UHS_SDR104)
        bus_mode_str = "UHS-SDR104";
    else if (bus_mode & SD_MODE_UHS_SDR50)
        bus_mode_str = "UHS-SDR50";
    else if (bus_mode & SD_MODE_UHS_SDR25)
        bus_mode_str = "UHS-SDR25";
    else if (bus_mode & SD_MODE_UHS_SDR12)
        bus_mode_str = "UHS-SDR12";
    else
        bus_mode_str = "bus mode unsupport";

    char *bus_width_str;
    if (card->host->bus_width == MMC_BUS_WIDTH_8)
        bus_width_str = "8";
    else if (card->host->bus_width == MMC_BUS_WIDTH_4)
        bus_width_str = "4";
    else if (card->host->bus_width == MMC_BUS_WIDTH_1)
        bus_width_str = "1";
    else
        bus_width_str = "unsupport";

    MMC_SD_DBG("%s: SD Card max rate=%dMHz, bus mode:%s, bus width:%s\n", card->host->name, max_rate / 1000000, bus_mode_str, bus_width_str);
#endif
}

static int mmc_all_send_cid(struct mmc_host *mmc, uint32_t *cid)
{
    return mmc_all_get_cid(mmc, cid);
}

/*
 * Given a 128-bit response, decode to our card CSD structure.
 */
static int mmc_sd_parse_csd(struct mmc_card *card)
{
    struct mmc_csd *csd = &card->csd;
    uint32_t *resp = card->raw_csd;
    uint32_t e, m;

    memset(csd, 0x00, sizeof(struct mmc_csd));

    csd->structure = UNSTUFF_BITS(resp, 126, 2);
    switch (csd->structure) {
    case 0:
        m = UNSTUFF_BITS(resp, 115, 4);
        e = UNSTUFF_BITS(resp, 112, 3);
        csd->taac           = (tacc_exp[e] * tacc_mant[m] + 9) / 10;
        csd->nsac           = UNSTUFF_BITS(resp, 104, 8);

        m = UNSTUFF_BITS(resp, 99, 4);
        e = UNSTUFF_BITS(resp, 96, 3);
        csd->tran_speed     = tran_exp[e] * tran_mant[m];
        csd->cmd_class      = UNSTUFF_BITS(resp, 84, 12);

        csd->c_size         = UNSTUFF_BITS(resp, 62, 12);
        csd->c_size_mult    = UNSTUFF_BITS(resp, 47, 3);

        csd->rd_blk_len     = UNSTUFF_BITS(resp, 80, 4);
        csd->rd_blk_part    = UNSTUFF_BITS(resp, 79, 1);
        csd->wr_blk_misalign= UNSTUFF_BITS(resp, 78, 1);
        csd->rd_blk_misalign= UNSTUFF_BITS(resp, 77, 1);
        csd->dsr_imp        = UNSTUFF_BITS(resp, 76, 1);
        csd->r2w_factor     = UNSTUFF_BITS(resp, 26, 3);
        csd->wr_blk_len     = UNSTUFF_BITS(resp, 22, 4);
        csd->wr_blk_partial = UNSTUFF_BITS(resp, 21, 1);
        csd->w_protect      = 0;

        if (UNSTUFF_BITS(resp, 46, 1)) {
            card->erase_size = 1;
        } else if (csd->wr_blk_len >= 9) {
            card->erase_size = UNSTUFF_BITS(resp, 39, 7) + 1;
            card->erase_size <<= csd->wr_blk_len - 9;
        }


        /* Device Capacity */
        m = csd->c_size;
        e = csd->c_size_mult;
        card->card_capacity = (m + 1) << (e + 2);
        break;

    case 1:
        /*
         * SDHC/SDXC协议使用block-addressed,传递块设备地址的未验证，
         * 待设备验证
         */

        /* 设置block addressed标志，在块设备的读写接口中检查该标志 */
        mmc_card_set_blockaddr(card);
        csd->taac           = 0; /* Unused */
        csd->nsac           = 0; /* Unused */

        m = UNSTUFF_BITS(resp, 99, 4);
        e = UNSTUFF_BITS(resp, 96, 3);
        csd->tran_speed     = tran_exp[e] * tran_mant[m];
        csd->cmd_class      = UNSTUFF_BITS(resp, 84, 12);

        csd->c_size         = UNSTUFF_BITS(resp, 48, 22);

        /* SDXC cards have a minimum C_SIZE of 0x00FFFF */
        if (csd->c_size >= 0xFFFF) {
            //mmc_card_set_ext_capacity(card);
        }

        csd->rd_blk_len     = 9;
        csd->rd_blk_part    = 0;
        csd->wr_blk_misalign= 0;
        csd->rd_blk_misalign= 0;
        csd->r2w_factor     = 4; /* Unuse */
        csd->wr_blk_len     = 9;
        csd->wr_blk_partial = 0;

        card->erase_size = 1;

        /* Device Capacity */
        m = UNSTUFF_BITS(resp, 48, 22);
        card->card_capacity = (1 + m) << 10;
        break;

    default:
        printf("%s unrecognised CSD structure version %d\n", card->host->name, csd->structure);
        return -EINVAL;

    }

    /*
     * 重新调整 Read/Write Block单位(Byte)
     */
    csd->rd_blk_len = 1 << csd->rd_blk_len;
    csd->wr_blk_len = 1 << csd->wr_blk_len;

    /* Card infomation */
    card->tacc_ns = csd->taac;
    card->tacc_clks = csd->nsac * 100;

    card->max_data_rate = csd->tran_speed;

    card->card_blk_size = csd->rd_blk_len;
    card->card_capacity *= card->card_blk_size;
    card->card_capacity >>= 10; /* unit:KByte */

    struct mmc_cid *cid = &card->cid;
    printf("SD-Nand Device:%c%c%c%c%c%c:%x Capacity %lld MB\n", cid->prod_name[0],
                                                    cid->prod_name[1],
                                                    cid->prod_name[2],
                                                    cid->prod_name[3],
                                                    cid->prod_name[4],
                                                    cid->prod_name[5],
                                                    card->rca,
                                                    card->card_capacity / 1024);
    return 0;
}

/*
 * Given the decoded CSD structure, decode the raw CID to our CID structure.
 */
static void mmc_sd_decode_cid(struct mmc_card *card)
{
    uint32_t *resp = card->raw_cid;
    struct mmc_cid *cid = &card->cid;

    memset(cid, 0x00, sizeof(struct mmc_cid));

    cid->manfid         = UNSTUFF_BITS(resp, 120, 8);
    cid->oemid          = UNSTUFF_BITS(resp, 104, 16);
    cid->prod_name[0]   = UNSTUFF_BITS(resp, 96, 8);
    cid->prod_name[1]   = UNSTUFF_BITS(resp, 88, 8);
    cid->prod_name[2]   = UNSTUFF_BITS(resp, 80, 8);
    cid->prod_name[3]   = UNSTUFF_BITS(resp, 72, 8);
    cid->prod_name[4]   = UNSTUFF_BITS(resp, 64, 8);
    cid->serial         = UNSTUFF_BITS(resp, 24, 32);
    cid->year           = UNSTUFF_BITS(resp, 12, 8);
    cid->month          = UNSTUFF_BITS(resp, 8, 4);

    cid->year += 2000;   /* SD Cards year offset */
}

/*
 * Given a 64-bit response, decode to our card SCR structure.
 */
static int mmc_decode_scr(struct mmc_card *card)
{
    struct sd_scr *scr = &card->scr;
    uint32_t scr_struct;
    uint32_t resp[4];

    resp[3] = card->raw_scr[1];
    resp[2] = card->raw_scr[0];

    scr_struct = UNSTUFF_BITS(resp, 60, 4);
    if (scr_struct != 0) {
        printf("%s unrecognised SCR structure version %d\n", card->host->name, scr_struct);
        return -EINVAL;
    }

    scr->sda_vsn = UNSTUFF_BITS(resp, 56, 4);
    scr->bus_widths = UNSTUFF_BITS(resp, 48, 4);
    if (scr->sda_vsn == SCR_SPEC_VER_2) {
        /* Check if Physical Layer Spec v3.0 is supported */
        scr->sda_spec3 = UNSTUFF_BITS(resp, 47, 1);
    }

    if (scr->sda_spec3)
        scr->cmds = UNSTUFF_BITS(resp, 32, 2);

    return 0;
}

static int mmc_sd_switch(struct mmc_card *card, int mode, int group,
    uint8_t value, uint8_t *resp)
{
    struct mmc_cmd cmd = {0};
    struct mmc_data data = {0};
    int ret;

    mode = !!mode;
    value &= 0x0F;

    cmd.opcode      =  SD_SWITCH;
    cmd.arg         =  mode << 31 | 0x00FFFFFF;
    cmd.arg         &= ~(0xF << (group * 4));
    cmd.arg         |= value << (group * 4);
    cmd.resp_type   =  MMC_RSP_R1 | MMC_CMD_ADTC;

    data.blksz      = 64;
    data.blocks     = 1;
    data.flags      = MMC_DATA_READ;
    data.dest       = (char *)resp;

    ret = mmc_send_cmd_data(card->host, &cmd, &data);
    if (ret < 0) {
        printf("%s send sd swtich command CMD%d failed\n",__FUNCTION__, cmd.opcode);
        return -1;
    }

    if (cmd.error)
        return cmd.error;

    if (data.error)
        return data.error;

    return 0;
}

int mmc_app_set_bus_width(struct mmc_card *card, int width)
{
    struct mmc_cmd cmd = {0};
    int ret;

    cmd.retries     = 3;
    cmd.opcode      = SD_APP_SET_BUS_WIDTH;
    cmd.resp_type   = MMC_RSP_R1 | MMC_CMD_AC;

    switch (width) {
    case MMC_BUS_WIDTH_1:
        cmd.arg     = SD_BUS_WIDTH_1;
        break;
    case MMC_BUS_WIDTH_4:
        cmd.arg     = SD_BUS_WIDTH_4;
        break;
    default:
        return -EINVAL;
    }

    ret = mmc_send_app_cmd_data(card->host, card, &cmd, NULL, cmd.retries);
    if (ret) {
        printf("%s: sd set bus width(%d) failed", card->host->name, width);
        return ret;
    }

    return 0;
}

static int mmc_app_sd_status(struct mmc_card *card, void *ssr)
{
    struct mmc_cmd cmd = {0};
    struct mmc_data data = {0};
    int ret;

    ret = mmc_app_cmd(card->host, card);
    if (ret)
        return ret;

    cmd.opcode      = SD_APP_SD_STATUS;
    cmd.arg         = 0;
    cmd.resp_type   = MMC_RSP_R1 | MMC_CMD_ADTC;

    data.blksz      = 64;
    data.blocks     = 1;
    data.flags      = MMC_DATA_READ;
    data.dest       = ssr;

    ret = mmc_send_cmd_data(card->host, &cmd, &data);
    if (ret < 0) {
        printf("%s send sd status command CMD%d failed\n",__FUNCTION__, cmd.opcode);
        return -1;
    }

    if (cmd.error)
        return cmd.error;

    if (data.error)
        return data.error;

    return 0;
}

static int mmc_app_send_scr(struct mmc_card *card, uint32_t *scr)
{
    struct mmc_cmd cmd = {0};
    struct mmc_data data = {0};
    int ret;
    void *data_buf;

    ret = mmc_app_cmd(card->host, card);
    if (ret)
        return ret;

    data_buf = cache_align_malloc(sizeof(card->raw_scr));
    if (data_buf == NULL)
        return -ENOMEM;

    cmd.opcode      = SD_APP_SEND_SCR;
    cmd.arg         = 0;
    cmd.resp_type   = MMC_RSP_R1 | MMC_CMD_ADTC;

    data.blksz      = 8;
    data.blocks     = 1;
    data.flags      = MMC_DATA_READ;
    data.dest       = data_buf;

    ret = mmc_send_cmd_data(card->host, &cmd, &data);
    if (ret < 0) {
        printf("%s send scr command CMD%d failed\n",__FUNCTION__, cmd.opcode);
        return -1;
    }

    memcpy(scr, data_buf, sizeof(card->raw_scr));
    free(data_buf);

    if (cmd.error)
        return cmd.error;
    if (data.error)
        return data.error;

    scr[0] = be32_to_cpu(scr[0]);
    scr[1] = be32_to_cpu(scr[1]);

    return 0;
}


static int mmc_read_ssr(struct mmc_card *card)
{
    uint32_t au, es, et, eo;
    int ret;
    int i;
    uint32_t *ssr;

    if (!(card->csd.cmd_class & CCC_APP_SPEC)) {
        printf("%s: card lacks mandatory SD Status function\n", card->host->name);
        return 0;
    }

    ssr = cache_align_malloc(64);
    if (ssr == NULL)
        return -ENOMEM;

    ret = mmc_app_sd_status(card, ssr);
    if (ret) {
        printf("%s: problem reading SD Status register\n", card->host->name);
        ret = 0;
        goto out;
    }

    for (i=0; i<16; i++)
        ssr[i] = be32_to_cpu(ssr[i]);

    /* decode SSR */
    au = UNSTUFF_BITS(ssr, 428 - 384, 4);
    if (au) {
        if (au <= 9 || card->scr.sda_spec3) {
            card->ssr.au = sd_au_size[au];
            es = UNSTUFF_BITS(ssr, 408 - 384, 16);
            et = UNSTUFF_BITS(ssr, 402 - 384, 6);
            if (es && et) {
                eo = UNSTUFF_BITS(ssr, 400 - 384, 2);
                card->ssr.erase_timeout = (et * 1000) / es;
                card->ssr.erase_offset = eo * 1000;
            }
        } else {
            printf("%s: SD Status: Invalid Allocation Unit size\n", card->host->name);
        }
    } /* end of if(au) ... */

out:
    free(ssr);
    return ret;
}

/*
 * Fetches and decodes switch information
 */
static int mmc_read_swtich(struct mmc_card *card)
{
    int ret;
    uint8_t *status;

    if (card->scr.sda_vsn < SCR_SPEC_VER_1)
        return 0;

    if (!(card->csd.cmd_class & CCC_SWITCH)) {
        printf("%s: card lacks mandatory switch function, performance might suffer\n", card->host->name);
        return 0;
    }

    ret = -EIO;

    status = cache_align_malloc(64);
    if (status == NULL) {
        printf("%s: could not allocate a buffer for switch capabilities\n", card->host->name);
        return -ENOMEM;
    }

    /*
     * Find out the card's support bits with a mode 0 operation.
     * The argument does not matter, as the support bits do not
     * change with the arguments.
     */
    ret = mmc_sd_switch(card, 0, 0, 0, status);
    if (ret) {
        /*
         * If the host or the card can't do the switch, fail more gracefully.
         */
        if (ret != -EINVAL && ret != -ENOSYS && ret != -EFAULT)
            goto out;

        printf("%s: problem reading Bus Speed modes\n", card->host->name);
        ret = 0;

        goto out;
    }

    if (status[13] & SD_MODE_HIGH_SPEED)
        card->sw_caps.hs_max_dtr = HIGH_SPEED_MAX_DTR;

    if (card->scr.sda_spec3) {
        card->sw_caps.sd3_bus_mode = status[13];
        /* Driver Strengths supported by the card */
        card->sw_caps.sd3_drv_type = status[9];
    }

out:
    free(status);

    return ret;
}

static int sd_set_bus_speed_mode(struct mmc_card *card, uint8_t *status)
{
    int ret;
    uint32_t timing = 0;

    switch (card->sd_bus_speed) {
    case UHS_SDR104_BUS_SPEED:
        timing = MMC_TIMING_UHS_SDR104;
        card->sw_caps.uhs_max_dtr = UHS_SDR104_MAX_DTR;
        break;
    case UHS_DDR50_BUS_SPEED:
        timing = MMC_TIMING_UHS_DDR50;
        card->sw_caps.uhs_max_dtr = UHS_DDR50_MAX_DTR;
        break;
    case UHS_SDR50_BUS_SPEED:
        timing = MMC_TIMING_UHS_SDR50;
        card->sw_caps.uhs_max_dtr = UHS_SDR50_MAX_DTR;
        break;
    case UHS_SDR25_BUS_SPEED:
        timing = MMC_TIMING_UHS_SDR25;
        card->sw_caps.uhs_max_dtr = UHS_SDR25_MAX_DTR;
        break;
    case UHS_SDR12_BUS_SPEED:
        timing = MMC_TIMING_UHS_SDR12;
        card->sw_caps.uhs_max_dtr = UHS_SDR12_MAX_DTR;
        break;
    default:
        return 0;
    }

    ret = mmc_sd_switch(card, 1, 0, card->sd_bus_speed, status);
    if (ret)
        return ret;

    if ((status[16] & 0xF) != card->sd_bus_speed)
        printf("%s: Problem setting bus speed mode!\n", card->host->name);
    else {
        mmc_set_timing(card->host, timing);
        mmc_set_clock(card->host, card->sw_caps.uhs_max_dtr);
    }

    return 0;
}

static int sd_set_current_limit(struct mmc_card *card, uint8_t *status)
{
    int current_limit = SD_SET_CURRENT_NO_CHANGE;
    int ret;
    uint32_t max_current;

    /*
     * Current limit switch is only defined for SDR50, SDR104, and DDR50
     * bus speed modes. For other bus speed modes, we do not change the
     * current limit.
     */
    if ((card->sd_bus_speed != UHS_SDR50_BUS_SPEED) &&
        (card->sd_bus_speed != UHS_SDR104_BUS_SPEED) &&
        (card->sd_bus_speed != UHS_DDR50_BUS_SPEED) )
        return 0;

    /*
     * Host has different current capabilities when operating at
     * different voltages, so find out its max current first.
     */
    max_current = sd_get_host_max_current(card->host);

    if (max_current >= 800)
        current_limit = SD_SET_CURRENT_LIMIT_800;
    else if (max_current >= 600)
        current_limit = SD_SET_CURRENT_LIMIT_600;
    else if (max_current >= 400)
        current_limit = SD_SET_CURRENT_LIMIT_400;
    else if (max_current >= 200)
        current_limit = SD_SET_CURRENT_LIMIT_200;

    if (current_limit != SD_SET_CURRENT_NO_CHANGE) {
        ret = mmc_sd_switch(card, 1, 3, current_limit, status);
        if (ret)
            return ret;

        if (((status[15] >> 4) & 0x0F) != current_limit)
            printf("%s: Problem setting current limit!\n", card->host->name);
    }

    return 0;
}

static void sd_update_bus_speed_mode(struct mmc_card *card)
{
    /*
     * If the host doesn't support any of the UHS-I modes, fallback on
     * default speed.
     */
    if (!mmc_host_uhs(card->host)) {
        card->sd_bus_speed = 0;
        return ;
    }

    if ( (card->host->capacity & MMC_CAP_UHS_SDR104) &&
        (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR104)) {

            card->sd_bus_speed = UHS_SDR104_BUS_SPEED;
    } else if ( (card->host->capacity & MMC_CAP_UHS_DDR50) &&
        (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_DDR50) ) {

            card->sd_bus_speed = UHS_DDR50_BUS_SPEED;
    } else if ( (card->host->capacity & (MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50) ) &&
        (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR50) ) {

            card->sd_bus_speed = UHS_SDR50_BUS_SPEED;
    } else if ( (card->host->capacity & (MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50 |
        MMC_CAP_UHS_SDR25) ) &&
        (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR25) ) {

            card->sd_bus_speed = UHS_SDR25_BUS_SPEED;
    } else if ( (card->host->capacity & (MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50 |
        MMC_CAP_UHS_SDR25 | MMC_CAP_UHS_SDR12) ) &&
        (card->sw_caps.sd3_bus_mode & SD_MODE_UHS_SDR12) ) {

            card->sd_bus_speed = UHS_SDR12_BUS_SPEED;
    }
}

/*
 * Test if the card supports high-speed mode and, if so, switch to it.
 */
int mmc_sd_switch_hs(struct mmc_card *card)
{
    int ret;
    uint8_t *status;

    if (card->scr.sda_vsn < SCR_SPEC_VER_1)
        return 0;

    if (!(card->csd.cmd_class & CCC_SWITCH))
        return 0;

    if (!(card->host->capacity & MMC_CAP_SD_HIGHSPEED)) {
        mmc_set_timing(card->host, MMC_TIMING_LEGACY);
        return 0;
    }

    if (card->sw_caps.hs_max_dtr == 0)
        return 0;


    status = cache_align_malloc(64);
    if (status == NULL) {
        printf("%s: could not allocate a buffer for switch capabilities\n", card->host->name);
        return -ENOMEM;
    }

    ret = mmc_sd_switch(card, 1, 0, 1, status);
    if (ret) {
        printf("%s: mmc sd switch hs failed. ret=%d\n", card->host->name, ret);
        goto out;
    }

    if ((status[16] & 0xF) != 1) {
        printf("%s: Problem switching card into high-speed mode!\n", card->host->name);
        ret = 0;
    } else {
        ret = 1;
    }

out:
    free(status);

    return ret;
}

uint32_t mmc_sd_get_max_clock(struct mmc_card *card)
{
    uint32_t max_dtr = (uint32_t)-1;

    if (mmc_card_hs(card->host)) {
        if (max_dtr > card->sw_caps.hs_max_dtr)
            max_dtr = card->sw_caps.hs_max_dtr;
    } else if (max_dtr > card->max_data_rate) {
        max_dtr = card->max_data_rate;
    }

    return max_dtr;
}

static int mmc_sd_get_ro(struct mmc_card *card)
{
    /* no write protect, can read & write */
    return 0;
}

/*
 * UHS-I specific initialization procedure
 */
static int mmc_sd_init_uhs_card(struct mmc_card *card)
{
    int ret;
    uint8_t *status;

    if (!card->scr.sda_spec3)
        return 0;

    if (!(card->csd.cmd_class & CCC_SWITCH))
        return 0;

    status = cache_align_malloc(64);
    if (status == NULL) {
        printf("%s: could not allocate a buffer for switch capabilities\n", card->host->name);
        return -ENOMEM;
    }

    /*
     * Set 4-bit bus width
     * 支持8线模式(eMMC)的配置 同时也支持4线
     */
    if ((card->host->capacity & (MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA) ) &&
        (card->scr.bus_widths & SD_SCR_BUS_WIDTH_4)) {

        ret = mmc_app_set_bus_width(card, MMC_BUS_WIDTH_4);
        if (ret)
            goto out;

        mmc_set_bus_width(card->host, MMC_BUS_WIDTH_4);
    }

    /*
     * Select the bus speed mode depending on host and card capability.
     */
    sd_update_bus_speed_mode(card);

    /*
     * Set the driver strength for the card(暂未实现)
     */

    /*
     * Set current limit for the card
     */
    ret = sd_set_current_limit(card, status);
    if (ret)
        goto out;

    /*
     * Set bus speed mode of the card
     */
    ret = sd_set_bus_speed_mode(card, status);
    if (ret)
        goto out;

    /*
     * tuning is only valid for SDR50 and SDR104 mode SD-cards.
     * Note that tuning is mandatory for SDR104.
     */
    if ((card->host->timing == MMC_TIMING_UHS_SDR50 ||
        card->host->timing == MMC_TIMING_UHS_DDR50 ||
        card->host->timing == MMC_TIMING_UHS_SDR104) ) {

        ret = mmc_execute_tuning(card);
        /*
         * CMD19 tuning is available for unlocked cards in transfer state of 1.8V signaling mode.
         * CMD19 tuning is also available for DDR50 mode.
         */
        if (ret && card->host->timing == MMC_TIMING_UHS_DDR50) {
            printf("%s: ddr50 tuning failed\n", card->host->name);
            ret = 0;
        }
    }

out:
    free(status);

    return ret;
}

static int mmc_sd_setup_card(struct mmc_card *card)
{
    int ret;

    /* Fetch SCR from card */
    ret = mmc_app_send_scr(card, card->raw_scr);
    if (ret)
        return ret;

    ret = mmc_decode_scr(card);
    if (ret)
        return ret;

    /*
     * Fetch and process SD Status register.
     */
    ret = mmc_read_ssr(card);
    if (ret)
        return ret;

    /*
     * Erase init depends on CSD and SSR
     * 设置可以擦除的块大小：设置为固定
     */
    //mmc_init_erase(card);

    /*
     * Fetch switch information from card.
     */
    ret = mmc_read_swtich(card);
    if (ret)
        return ret;

    /*
     * Check if read-only switch is active.
     */
    int ro = mmc_sd_get_ro(card);
    if (ro < 0) {
        printf("%s: warn: host does not support reading read-only switch, assuming write-enable\n", card->host->name);
    }

    return 0;
}

static int mmc_sd_get_cid(struct mmc_host *mmc, uint32_t ocr, uint32_t *cid, uint32_t *rocr)
{
    int ret;
    int max_current;
    uint32_t pocr = ocr;
    int retries = 3;

try_again:
    if (!retries) {
        ocr &= ~SD_OCR_S18R;
        printf("%s Skipping voltage switch\n", mmc->name);
    }

    /*
     * Since we're changing the OCR value, we seem to
     * need to tell some cards to go back to the idle
     * state.  We wait 1ms to give cards time to
     * respond.
     */
    mmc_go_idle(mmc);

    /*
     * If SD_SEND_IF_COND indicates an SD 2.0
     * compliant card and we should set bit 30
     * of the ocr to indicate that we can handle
     * block-addressed SDHC cards.
     */
    ret = mmc_send_if_cond(mmc, ocr);
    if (!ret)
        ocr |= SD_OCR_CCS;

	/*
     * If the host supports one of UHS-I modes, request the card
     * to switch to 1.8V signaling level. If the card has failed
     * repeatedly to switch however, skip this.
     */
    if (retries && mmc_host_uhs(mmc))
        ocr |= SD_OCR_S18R;

    /*
     * If the host can supply more than 150mA at current voltage,
     * XPC should be set to 1.
     */
    max_current = sd_get_host_max_current(mmc);
    if (max_current > 150)
        ocr |= SD_OCR_XPC;

    ret = mmc_send_app_op_cond(mmc, ocr, rocr);
    if (ret)
        return ret;

    if (rocr &&  (*rocr & 0x41000000) == 0x41000000) {
        ret = mmc_set_signal_voltage(mmc, MMC_SIGNAL_VOLTAGE_180, pocr);
        if (ret == -EAGAIN) {
            retries--;
            goto try_again;
        } else if (ret) {
            retries = 0;
            goto try_again;
        }
    }

    ret = mmc_all_send_cid(mmc, cid);

    return ret;
}

static int mmc_sd_init_card(struct mmc_host *mmc, uint32_t ocr)
{
    int ret;
    uint32_t cid[4];
    uint32_t rocr = 0;
    struct mmc_card *card;

    ret = mmc_sd_get_cid(mmc, ocr, cid, &rocr);
    if (ret) {
        printf("%s sd get cid failed\n", mmc->name);
        return ret;
    }

    card = malloc(sizeof(struct mmc_card));
    if (card == NULL) {
        printf("malloc SD card failed.\n");
        ret = -ENOMEM;
        goto err;
    }

    memset(card, 0x00, sizeof(struct mmc_card));
    card->type = MMC_TYPE_SD;
    card->host = mmc;
    memcpy(card->raw_cid, cid, sizeof(card->raw_cid));

    /*
     * For native busses: get card RCA and quit open drain mode.
     */
    ret = mmc_get_card_relative_addr(mmc, &card->rca);
    if (ret < 0)
        goto err1;

    /*
     * Get Card CSD information from SD (Card-Specific Data)
     */
    ret = mmc_get_csd(card, card->raw_csd);
    if (ret)
        goto err1;

    mmc_sd_decode_cid(card);

    ret = mmc_sd_parse_csd(card);
    if (ret)
        goto err1;

    /*
     * Select card, as all following commands rely on that.
     */
    ret = mmc_select_card(card);
    if (ret)
        goto err1;

    ret = mmc_sd_setup_card(card);
    if (ret)
        goto err1;

    __mmc_debug_dump_device_info(card);
    /* Initialization sequence for UHS-I cards */
    if (rocr & SD_ROCR_S18A) {
        /* TODO:待验证 */
        ret = mmc_sd_init_uhs_card(card);
        if (ret)
            goto err1;

    } else {
        /*
         * Attempt to change to high-speed (if supported)
         */
        ret = mmc_sd_switch_hs(card);
        if (ret > 0)
            mmc_set_timing(card->host, MMC_TIMING_SD_HS);
        else if (ret)
            goto err1;

        /*
         * Set bus speed.
         */
        mmc_set_clock(card->host, mmc_sd_get_max_clock(card));

        /*
         * Switch to wider bus (if supported).
         * 支持8线模式(eMMC)的配置 同时也支持4线
         */
        if ((card->host->capacity & (MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA) ) &&
            (card->scr.bus_widths & SD_SCR_BUS_WIDTH_4)) {
            ret = mmc_app_set_bus_width(card, MMC_BUS_WIDTH_4);
            if (ret)
                goto err1;

            mmc_set_bus_width(card->host, MMC_BUS_WIDTH_4);
        }
    }

    __mmc_debug_dump_device_configure(card);

    /* card ops */
    card->ops = malloc(sizeof(struct mmc_card_ops));
    if (card->ops == NULL) {
        printf("malloc sd card ops failed.\n");
        ret = -ENOMEM;
        goto err2;
    }

    mmc_blk_set_ops(card->ops);

    mmc->card = card;

    return 0;

err2:
err1:
    free(card);
err:
    return ret;
}

int mmc_attach_sd(struct mmc_host *mmc)
{
    int ret = 0;
    uint32_t ocr, rocr;

    ret = mmc_send_app_op_cond(mmc, 0, &ocr);
    if (ret)
        return ret;

    /*
     * Can we support the voltage(s) of the card(s)?
     */
    rocr = mmc_select_voltage(mmc, ocr);
    if (!rocr) {
        ret = -EINVAL;
        return ret;
    }

	/*
     * Detect and init the SD Card.
     */
    ret = mmc_sd_init_card(mmc, rocr);
    if (ret) {
        printf("SD Card init failed\n");
        return -ENODEV;
    }

    printf("detect SD Successfully\n");

    return ret;
}


int mmc_attach_sd_params(struct mmc_host *mmc, struct card_info_params *params)
{
    int ret;
    struct mmc_card *card = NULL;

    assert(params);

    card = malloc(sizeof(struct mmc_card));
    if (card == NULL) {
        printf("malloc mmc card failed.\n");
        ret = -ENOMEM;
        goto err;
    }

    memset(card, 0x00, sizeof(struct mmc_card));
    card->type = MMC_TYPE_SD;
    card->rca = params->rca;
    card->host = mmc;

    memcpy(card->raw_cid, params->raw_cid, sizeof(card->raw_cid));
    mmc_sd_decode_cid(card);

    memcpy(card->raw_csd, params->raw_csd, sizeof(card->raw_cid));
    ret = mmc_sd_parse_csd(card);
    if (ret)
        goto err1;

    if (params->highcap)
        mmc_card_set_blockaddr(card);

    mmc->bus_width = params->bus_width;
    mmc->clock = params->max_speed;

    if (mmc->clock > 26 * 1000000)
        mmc->timing = MMC_TIMING_MMC_HS;        /* 26M ~ 52M */
    else
        mmc->timing = MMC_TIMING_LEGACY;        /* 0M ~ 26M */

    mmc->power_mode = MMC_POWER_ON;
    mmc->min_voltage = fls(mmc->voltages) - 1;

    /* 更新变量信息到控制器(mmc- soc-type.c) */
    mmc_set_timing(mmc, mmc->timing);

    /* card ops */
    card->ops = malloc(sizeof(struct mmc_card_ops));
    if (card->ops == NULL) {
        printf("malloc card ops failed.\n");
        ret = -ENOMEM;
        goto err1;
    }

    mmc_blk_set_ops(card->ops);

    mmc->card = card;

    return 0;

err1:
    free(card);
err:
    return ret;
}
