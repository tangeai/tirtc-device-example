/*
 * Copyright (c) 2021, Ingenic Semiconductor
 *
 */
#include <common.h>
#include <stdio.h>
#include <ffs.h>
#include "mmc-core.h"
#include "mmc-sd.h"
#include "mmc-g32s10x-regs.h"
#include "mmc-g32s10x-hal.h"

// #define DUPM_DEVICE_PARAMS_INFO
// #define MMC_DEBUG

#ifdef MMC_DEBUG
#define MMC_CORE_DBG(...)               printf("[MMC-CORE]"), printf(__VA_ARGS__)
#define MMC_CORE_WARN(...)              printf("[MSC-CORE]"), printf(__VA_ARGS__)
#define MMC_CORE_ERR(...)               printf("[MSC-CORE] Err:"), printf(__VA_ARGS__)
#else
#define MMC_CORE_DBG(...)
#define MMC_CORE_WARN(...)
#define MMC_CORE_ERR(...)               printf("[MSC-CORE] Err:"), printf(__VA_ARGS__)
#endif

static struct card_info_params *card_params;

static const unsigned retry_freqs[] = { 400000, 300000, 200000, 100000 };

int mmc_send_if_cond(struct mmc_host *mmc, uint32_t ocr);
int sdio_reset(struct mmc_host *mmc);

int mmc_attach_sd(struct mmc_host *mmc);
int mmc_attach_sdio(struct mmc_host *mmc);
int mmc_attach_mmc(struct mmc_host *mmc);

int mmc_attach_mmc_params(struct mmc_host *mmc, struct card_info_params *params);
int mmc_attach_sd_params(struct mmc_host *mmc, struct card_info_params *params);


/*
 * MMC operations
 */
int mmc_send_cmd_data(struct mmc_host *mmc, struct mmc_cmd* cmd, struct mmc_data* data)
{
    int err = 0;

    do {
        mmc->send_cmd_data(mmc, cmd, data);

        if (cmd->error && cmd->retries) {
            err = cmd->error;
            MMC_CORE_WARN("%s: send cmd failed (CMD%u): %d, retrying...\n", mmc->name, cmd->opcode, cmd->error);
        }

        if (data && data->error) {
            err = data->error;
            MMC_CORE_WARN("%s: send data failed (CMD%u): %d, retrying...\n", mmc->name, cmd->opcode, data->error);
        }
    } while (err && cmd->retries--);

    return err;
}

int mmc_send_status(struct mmc_card *card, uint32_t *status)
{
    struct mmc_host *mmc = card->host;
    struct mmc_cmd cmd = {0};
    int ret = 0;

    cmd.opcode          = MMC_SEND_STATUS;
    cmd.arg             = card->rca << 16;
    cmd.resp_type       = MMC_RSP_R1 | MMC_CMD_AC;

    ret = mmc_send_cmd_data(mmc, &cmd, NULL);
    if (ret)
        return ret;

    if (status)
        *status = cmd.resp[0];

    return 0;
}

int mmc_go_idle(struct mmc_host *mmc)
{
    struct mmc_cmd cmd = {0};
    int ret = 0;

    cmd.opcode      = MMC_GO_IDLE_STATE;
    cmd.arg         = 0;
    cmd.resp_type   = MMC_RSP_NONE;

    ret = mmc_send_cmd_data(mmc, &cmd, NULL);
    if (ret)
        return ret;

    return 0;
}

int mmc_execute_tuning(struct mmc_card *card)
{
    int ret;
    int opcode;

    if (!card->host->execute_tuning)
        return 0;

    if (card->type == MMC_TYPE_MMC)
        opcode = MMC_SEND_TUNING_BLOCK_HS200;
    else
        opcode = MMC_SEND_TUNING_BLOCK;

    ret = card->host->execute_tuning(card->host, opcode);
    if (ret < 0)
        printf("%s tuning execution failed\n", card->host->name);

    return ret;
}

static inline void mmc_set_ios(struct mmc_host *mmc)
{
    mmc->set_ios(mmc);
}

void mmc_set_bus_width(struct mmc_host *mmc, uint32_t width)
{
    mmc->bus_width = width;
    mmc_set_ios(mmc);
}

void mmc_set_clock(struct mmc_host *mmc, uint32_t clock)
{
    if (clock > mmc->f_max)
        clock = mmc->f_max;

    if (clock < mmc->f_min)
        clock = mmc->f_min;

    mmc->clock = clock;
    mmc_set_ios(mmc);
}

void mmc_set_timing(struct mmc_host *mmc, uint32_t timing)
{
    mmc->timing = timing;
    mmc_set_ios(mmc);
}

int __mmc_set_signal_voltage(struct mmc_host *mmc, int signal_voltage)
{
    /*
     * Signal Voltage Switching is only applicable for Host Controllers
     * v3.00 and above.
     */
    if (mmc->version < SDHCI_SPEC_300) {
        MMC_CORE_WARN("%s: not support voltage_switch\n", mmc->name);
        return 0;
    }

    switch (signal_voltage) {
    case MMC_SIGNAL_VOLTAGE_330:
        /* Set 1.8V Signal Enable in the Host Control2 register to 0 */
        mmc_hal_enable_tuned_clk(mmc->index, 0);

        /* Some controller need to do more when switching */
        mmc_voltage_switch(mmc->index, 0);

        /* Wait for 5ms */
        msleep(5);

        /* 3.3V regulator output should be stable within 5 ms */
        if (mmc_hal_get_tuned_clk(mmc->index))
            return 0;

        MMC_CORE_WARN("%s: 3.3V regulator output did not became stable\n", mmc->name);

    case MMC_SIGNAL_VOLTAGE_180:
        /*
         * Enable 1.8V Signal Enable in the Host Control2
         * register
         */
        mmc_hal_enable_tuned_clk(mmc->index, 1);

        /* Some controller need to do more when switching */
        mmc_voltage_switch(mmc->index, 1);

        /* 1.8V regulator output should be stable within 5 ms */
        if (mmc_hal_get_tuned_clk(mmc->index))
            return 0;

        MMC_CORE_WARN("%s: 1.8V regulator output did not became stable\n", mmc->name);
    case MMC_SIGNAL_VOLTAGE_120:
    default:
        return 0;
    }

    return -EAGAIN;
}

int mmc_set_signal_voltage(struct mmc_host *mmc, int signal_voltage, uint32_t ocr)
{
    if (!mmc_voltage_1v8_check(mmc->index)) {
        printf("[MSC%d] not support set signal voltage!!!\n", mmc->index);
        return -1;
    }

	struct mmc_cmd cmd = {0};
	int err = 0;

	assert(mmc);

	/*
	 * Send CMD11 only if the request is to switch the card to
	 * 1.8V signalling.
	 */
	if (signal_voltage == MMC_SIGNAL_VOLTAGE_330)
		return __mmc_set_signal_voltage(mmc, signal_voltage);

	cmd.opcode = SD_SWITCH_VOLTAGE;
	cmd.arg = 0;
	cmd.resp_type = MMC_RSP_R1 | MMC_CMD_AC;

	err = mmc_send_cmd_data(mmc, &cmd, NULL);
	if (err)
		return err;

	if ((cmd.resp[0] & R1_ERROR))
		return -EIO;

	/*
	 * The card should drive cmd and dat[0:3] low immediately
	 * after the response of cmd11, but wait 1 ms to be sure
	 */
	mdelay(1);
	if (!!(mmc_hal_get_presend_status(mmc->index) & 0xF00000)) {
		err = -EAGAIN;
		goto power_cycle;
	}
	/*
	 * During a signal voltage level switch, the clock must be gated
	 * for 5 ms according to the SD spec
	 */
    mmc_hal_clock_control_enable(mmc->index, 0);

	if (__mmc_set_signal_voltage(mmc, signal_voltage)) {
		/*
		 * Voltages may not have been switched, but we've already
		 * sent CMD11, so a power cycle is required anyway
		 */
		err = -EAGAIN;
		goto power_cycle;
	}

	/* Keep clock gated for at least 10 ms, though spec only says 5 ms */
	mdelay(10);
    mmc_hal_clock_control_enable(mmc->index, 1);

	/* Wait for at least 1 ms according to spec */
	mdelay(1);

	/*
	 * Failure to switch is indicated by the card holding
	 * dat[0:3] low
	 */
	if (!(mmc_hal_get_presend_status(mmc->index) & 0xF00000))
		err = -EAGAIN;

power_cycle:
	if (err) {
		MMC_CORE_DBG("%s: Signal voltage switch failed, "
			"power cycling card\n", mmc->name);
		mmc_power_cycle(mmc, ocr);
	}

	return err;
}

/*
 * Mask off any voltages we don't support and select
 * the lowest voltage
 */
int mmc_select_voltage(struct mmc_host *mmc, uint32_t ocr)
{
    int bit;

    MMC_CORE_DBG("mmc_select_voltage == 0x%x   ocr=0x%x\n", mmc->voltages, ocr);

    ocr = mmc->voltages & ocr;

    bit = __ffs(ocr);
    MMC_CORE_DBG(" new ocr=0x%x   bit=0x%x\n", ocr, bit);

    if (bit) {
        /*
         * 允许电压范围有波动
         */
        ocr &= 3 << bit;

    } else {
        MMC_CORE_WARN("host does not support card's voltages!\n");
        ocr = 0;
    }

    return ocr;
}

/******************************************************************************
 *
 *****************************************************************************/
static void mmc_power_on(struct mmc_host *mmc, uint32_t ocr)
{
    if (mmc->power_mode == MMC_POWER_ON)
        return ;

    mmc->bus_width  = MMC_BUS_WIDTH_1;
    mmc->power_mode = MMC_POWER_ON;
    mmc->min_voltage = fls(ocr) - 1;

    mmc_set_ios(mmc);
}

static void mmc_power_off(struct mmc_host *mmc)
{
    if (mmc->power_mode == MMC_POWER_OFF)
        return ;

    mmc->clock = 0;
    mmc->bus_width  = MMC_BUS_WIDTH_1;
    mmc->power_mode = MMC_POWER_OFF;

    mmc_set_ios(mmc);
}

void mmc_power_cycle(struct mmc_host *mmc, u32 ocr)
{
    mmc_power_off(mmc);
    /* Wait at least 1 ms according to SD spec */
    mdelay(1);
    mmc_power_on(mmc, ocr);
}

#ifdef DUPM_DEVICE_PARAMS_INFO
static void mmc_attch_dump_params(struct card_info_params *params)
{
    printf("Magic       : 0x%08x\n", params->magic);
    printf("Version     : 0x%08x\n", params->version);
    printf("Type        : 0x%08x\n", params->type);
    printf("highcap     : 0x%08x\n", params->highcap);
    printf("rca         : 0x%08x\n", params->rca);
    printf("bus_width   : 0x%08x\n", params->bus_width);
    printf("max_speed   : %d\n", params->max_speed);

    printf("================Dump CID=======================\n");
    printf("CID[0]      : 0x%08x\n", params->raw_cid[0]);
    printf("CID[1]      : 0x%08x\n", params->raw_cid[1]);
    printf("CID[2]      : 0x%08x\n", params->raw_cid[2]);
    printf("CID[3]      : 0x%08x\n", params->raw_cid[3]);
    printf("\n");

    printf("================Dump CSD=======================\n");
    printf("CSD[0]      : 0x%08x\n", params->raw_csd[0]);
    printf("CSD[1]      : 0x%08x\n", params->raw_csd[1]);
    printf("CSD[2]      : 0x%08x\n", params->raw_csd[2]);
    printf("CSD[3]      : 0x%08x\n", params->raw_csd[3]);
    printf("\n");

    printf("================Dump EXT_CSD====================\n");
    unsigned char *ext_csd = params->ext_csd;
    int i = 0;
    for (i=0; i < 512; i++) {
        if ( (i != 0) && (i % 16 == 0) ) {
            printf("\n");
        }
        printf("%02x:", ext_csd[i]);
    }
    printf("\n");
}
#endif

/*
 * 检查spl 中保存的mmc参数信息是否有效
 * =0: 有效
 * <0: 无效
 */
static int mmc_attch_params_address_is_valid(uint32_t address)
{
    if ((address != 0) && (address < 0x80000000 || address > 0xc0000000 - 1) ) {
        printf("card params address is invalid: 0x%x\n", address);
        return -1;
    }

    return 0;
}

/*
 * 检查spl 中保存的mmc参数信息是否有效
 * =0: 有效
 * <0: 无效
 */
static int mmc_attch_params_check_magic(uint32_t magic)
{
    if (magic != 0x534f5452) {
        printf("mmc attch params magic is invaild: 0x%x\n", magic);
        return -1;
    }
    return 0;
}

/*
 * 解析spl 中传入的mmc参数信息
 * =0: 解析成功
 * <0: 解析无效/失败
 */
static int mmc_attch_params(struct mmc_host *mmc, struct card_info_params *params)
{
    int type;
    int ret;

    ret = mmc_attch_params_address_is_valid((uint32_t)params);
    if (ret < 0)
        return ret;

#ifdef DUPM_DEVICE_PARAMS_INFO
    mmc_attch_dump_params(params);
#endif

    ret = mmc_attch_params_check_magic(params->magic);
    if (ret < 0)
        return ret;

    type = params->type;
    switch (type) {
    case MMC_TYPE_MMC:
        ret = mmc_attach_mmc_params(mmc, params);
        break;

    case MMC_TYPE_SD:
        ret = mmc_attach_sd_params(mmc, params);
        break;

    default:
        printf("mmc only support MMC_TYPE_MMC/MMC_TYPE_SD. not support type:%d\n", type);
        ret = -1;
        break;
    }

    return ret;
}

static int mmc_rescan_try_freq(struct mmc_host *mmc, uint32_t freq)
{
    int ret = -1;

    card_params = (struct card_info_params *)mmc->card_params;
    if (card_params)
        ret = mmc_attch_params(mmc, card_params);

    /* SPL参数解析初始化成功 */
    if (!ret)
        return 0;

    /*
     * 发送命令 执行重新初始化序列
     */
    mmc->clock = freq;

    MMC_CORE_DBG("%s: %s: trying to init card at %u Hz\n",
            mmc->name, __FUNCTION__, mmc->clock);

    mmc_power_on(mmc, mmc->voltages);

    /*
     * sdio_reset sends CMD52 to reset card.  Since we do not know
     * if the card is being re-initialized, just send it.  CMD52
     * should be ignored by SD/eMMC cards.
     */
    sdio_reset(mmc);

    mmc_go_idle(mmc);

    /* For SD Version 2.0 */
    mmc_send_if_cond(mmc, mmc->voltages);

    /*
     * Order's important: probe SDIO, then SD, then MMC
     */
    if (!mmc_attach_sdio(mmc))
        return 0;

    if (!mmc_attach_sd(mmc))
        return 0;

    if (!mmc_attach_mmc(mmc))
        return 0;

    mmc_power_off(mmc);

    return -EIO;
}

static int mmc_rescan(struct mmc_host *mmc)
{
    int i = 0;

    for (i = 0; i < ARRAY_SIZE(retry_freqs); i++) {
        if (!mmc_rescan_try_freq(
                mmc, max((uint32_t)retry_freqs[i], mmc->f_min))) {
            break;
        }
    }

    if (i >= ARRAY_SIZE(retry_freqs)) {
        printf("%s: mmc not find devices\n", mmc->name);
        return -1;
    }

    return 0;
}

static void mmc_detect_change_thread(void *data)
{
    struct mmc_host *mmc = (struct mmc_host *)data;
    thread_cond_signal(&mmc->dete_change_finish);

    while (1) {
        mutex_lock(&mmc->mutex);
        thread_cond_wait(&mmc->dete_change_cond, &mmc->mutex);

        int status = mmc->get_card_stauts(mmc);
        if (status) {
            /*
             * insert
             */
            if (mmc->card == NULL)
                mmc_rescan(mmc);
            /* end of insert */

        } else {

            /*
             * remove
             */
            if (mmc->card) {
                mmc_power_off(mmc);
                free(mmc->card->ops);
                free(mmc->card);
                mmc->card = NULL;
            }
            /* end of remove */

        } /* end of if ... else ... */

        thread_cond_signal(&mmc->dete_change_finish);

        mutex_unlock(&mmc->mutex);

    } /* end of while(1) */
}


int mmc_core_init(struct mmc_host *mmc)
{
    mutex_init(&mmc->mutex);
    thread_cond_init(&mmc->dete_change_cond);
    thread_cond_init(&mmc->dete_change_finish);

    thread_create("mmc-dete-change", 2048, mmc_detect_change_thread, mmc);

    /* wait the thread is running */
    mutex_lock(&mmc->mutex);
    thread_cond_wait(&mmc->dete_change_finish, &mmc->mutex);
    mutex_unlock(&mmc->mutex);

    return 0;
}

void mmc_detect_change(struct mmc_host *mmc)
{
    mutex_lock(&mmc->mutex);

    thread_cond_signal(&mmc->dete_change_cond);

    /* 等待mmc扫描完成:mmc->card值有效 */
    thread_cond_wait_timeout(&mmc->dete_change_finish, &mmc->mutex, 10 * 1000);

    mutex_unlock(&mmc->mutex);

}
