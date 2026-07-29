#include <driver/clk.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <common.h>
#include <os.h>
#include <soc/camera_sensor.h>

#define GC0308_FLAG_END                 0x00
#define GC0308_FLAG_DELAY               0xff
#define GC0308_PAGE_REG                 0xfe
#define GC0308_CHIP_ID                  0x9b
#define REG_PAGE                        0xfe
#define GC0308_VB_H                     0x0f
#define GC0308_VB_L                     0x02

#define GC0308_EXP_H                    0x03
#define GC0308_EXP_L                    0x04

#define GC0308_GLOBAL_GAIN              0x50
#define GC0308_PREGAIN                  0x51
#define GC0308_POSTGAIN                 0x52

#define I2C_ADDR                        0x21
#define ENDMARKER                       { 0xff, 0xff }

#define GC0308_WIDTH                    640
#define GC0308_HEIGHT                   480
#define VTS                             (GC0308_HEIGHT + 0x000c)
#define HTS                             (GC0308_WIDTH + 0x00d4)
#define FPS                             30

struct regval_list {
    unsigned char reg_num;
    unsigned char value;
};

static int power_gpio   = CONFIG_G32S10X_GC0308_DVP_GPIO_POWER; // GPIO_PB(12);
static int reset_gpio   = CONFIG_G32S10X_GC0308_DVP_GPIO_RESET; // GPIO_PA(30);
static int pwdn_gpio    = CONFIG_G32S10X_GC0308_DVP_GPIO_PWDN;  //  -1;
static int bus_num      = CONFIG_G32S10X_GC0308_DVP_I2C_BUSNUM; // 0

static struct i2c_device *i2c_dev;

/*
 * Interface    : DVP
 * MCLK         : 24Mhz,
 * resolution   : 640*480
 * FrameRate    : 25fps
 */
#if 1 // 1: yuv init, 0: raw init
static struct regval_list gc0308_init_regs[] = {
    {0xfe, 0x80},
    {0xfe, 0x00}, // set page0
    {0xd2, 0x10}, // close AEC
    {0x22, 0x55}, // close AWB

    {0x03, 0x01},
    {0x04, 0x2c},
    {0x5a, 0x56},
    {0x5b, 0x40},
    {0x5c, 0x4a},

    {0x22, 0x57}, // Open AWB

    {0xfe, 0x00},
    {0x01, 0x6a},  //hb
#ifndef CONFIG_RUN_ON_FPGA
    {0x02, 0x0c},
    {0x0f, 0x00},
#else
    {0x02, 0xbc}, //vb
    {0x0f, 0x70}, //vb hb high 4 bit
#endif

    // {0xe2,0x00},
    {0xe3, 0x96},

    {0xe4, 0x01},
    {0xe5, 0x2c},
    {0xe6, 0x01},
    {0xe7, 0x2c},
    {0xe8, 0x01},
    {0xe9, 0x2c},
    {0xea, 0x01},
    {0xeb, 0x2c},

    {0x05, 0x00},
    {0x06, 0x00},
    {0x07, 0x00},
    {0x08, 0x00},
    {0x09, 0x01},
    {0x0a, 0xe8},
    {0x0b, 0x02},
    {0x0c, 0x88},
    {0x0d, 0x02},
    {0x0e, 0x02},
    {0x10, 0x22},
    {0x11, 0xfd},
    {0x12, 0x2a},
    {0x13, 0x00},

    {0x15, 0x0a},
    {0x16, 0x05},
    {0x17, 0x01},
    {0x18, 0x44},
    {0x19, 0x44},
    {0x1a, 0x1e},
    {0x1b, 0x00},
    {0x1c, 0xc1},
    {0x1d, 0x08},
    {0x1e, 0x60},
    {0x1f, 0x03}, // 16

    {0x20, 0xff},
    {0x21, 0xf8},
    {0x22, 0x57},
    //{0x24, 0xb1}, // only Y
    {0x24, 0xa2}, // yuv422
    {0x28, 0x00}, // add

    {0x26, 0x03}, // 03
    {0x2f, 0x01},
    {0x30, 0xf7},
    {0x31, 0x50},
    {0x32, 0x00},
    {0x39, 0x04},
    {0x3a, 0x18},
    {0x3b, 0x20},
    {0x3c, 0x00},
    {0x3d, 0x00},
    {0x3e, 0x00},
    {0x3f, 0x00},
    {0x50, 0x10},
    {0x53, 0x82},
    {0x54, 0x80},
    {0x55, 0x80},
    {0x56, 0x82},
    {0x8b, 0x40},
    {0x8c, 0x40},
    {0x8d, 0x40},
    {0x8e, 0x2e},
    {0x8f, 0x2e},
    {0x90, 0x2e},
    {0x91, 0x3c},
    {0x92, 0x50},
    {0x5d, 0x12},
    {0x5e, 0x1a},
    {0x5f, 0x24},
    {0x60, 0x07},
    {0x61, 0x15},
    {0x62, 0x08},
    {0x64, 0x03},
    {0x66, 0xe8},
    {0x67, 0x86},
    {0x68, 0xa2},
    {0x69, 0x18},
    {0x6a, 0x0f},
    {0x6b, 0x00},
    {0x6c, 0x5f},
    {0x6d, 0x8f},
    {0x6e, 0x55},
    {0x6f, 0x38},
    {0x70, 0x15},
    {0x71, 0x33},
    {0x72, 0xdc},
    {0x73, 0x80},
    {0x74, 0x02},
    {0x75, 0x3f},
    {0x76, 0x02},
    {0x77, 0x20},
    {0x78, 0x88},
    {0x79, 0x81},
    {0x7a, 0x81},
    {0x7b, 0x22},
    {0x7c, 0xff},
    {0x93, 0x48},
    {0x94, 0x00},
    {0x95, 0x05},
    {0x96, 0xe8},
    {0x97, 0x40},
    {0x98, 0xf0},
    {0xb1, 0x38},
    {0xb2, 0x38},
    {0xbd, 0x38},
    {0xbe, 0x36},
    {0xd0, 0xc9},
    {0xd1, 0x10},

    {0xd3, 0x80},
    {0xd5, 0xf2},
    {0xd6, 0x16},
    {0xdb, 0x92},
    {0xdc, 0xa5},
    {0xdf, 0x23},
    {0xd9, 0x00},
    {0xda, 0x00},
    {0xe0, 0x09},

    {0xed, 0x04},
    {0xee, 0xa0},
    {0xef, 0x40},
    {0x80, 0x03},
    {0x80, 0x03},
    {0x9F, 0x10},
    {0xA0, 0x20},
    {0xA1, 0x38},
    {0xA2, 0x4E},
    {0xA3, 0x63},
    {0xA4, 0x76},
    {0xA5, 0x87},
    {0xA6, 0xA2},
    {0xA7, 0xB8},
    {0xA8, 0xCA},
    {0xA9, 0xD8},
    {0xAA, 0xE3},
    {0xAB, 0xEB},
    {0xAC, 0xF0},
    {0xAD, 0xF8},
    {0xAE, 0xFD},
    {0xAF, 0xFF},
    {0xc0, 0x00},
    {0xc1, 0x10},
    {0xc2, 0x1C},
    {0xc3, 0x30},
    {0xc4, 0x43},
    {0xc5, 0x54},
    {0xc6, 0x65},
    {0xc7, 0x75},
    {0xc8, 0x93},
    {0xc9, 0xB0},
    {0xca, 0xCB},
    {0xcb, 0xE6},
    {0xcc, 0xFF},
    {0xf0, 0x02},
    {0xf1, 0x01},
    {0xf2, 0x01},
    {0xf3, 0x30},
    {0xf9, 0x9f},
    {0xfa, 0x78},

    //---------------------------------------------------------------
    {0xfe, 0x01}, // set page1

    {0x00, 0xf5},
    {0x02, 0x1a},
    {0x0a, 0xa0},
    {0x0b, 0x60},
    {0x0c, 0x08},
    {0x0e, 0x4c},
    {0x0f, 0x39},
    {0x11, 0x3f},
    {0x12, 0x72},
    {0x13, 0x13},
    {0x14, 0x42},
    {0x15, 0x43},
    {0x16, 0xc2},
    {0x17, 0xa8},
    {0x18, 0x18},
    {0x19, 0x40},
    {0x1a, 0xd0},
    {0x1b, 0xf5},
    {0x70, 0x40},
    {0x71, 0x58},
    {0x72, 0x30},
    {0x73, 0x48},
    {0x74, 0x20},
    {0x75, 0x60},
    {0x77, 0x20},
    {0x78, 0x32},
    {0x30, 0x03},
    {0x31, 0x40},
    {0x32, 0xe0},
    {0x33, 0xe0},
    {0x34, 0xe0},
    {0x35, 0xb0},
    {0x36, 0xc0},
    {0x37, 0xc0},
    {0x38, 0x04},
    {0x39, 0x09},
    {0x3a, 0x12},
    {0x3b, 0x1C},
    {0x3c, 0x28},
    {0x3d, 0x31},
    {0x3e, 0x44},
    {0x3f, 0x57},
    {0x40, 0x6C},
    {0x41, 0x81},
    {0x42, 0x94},
    {0x43, 0xA7},
    {0x44, 0xB8},
    {0x45, 0xD6},
    {0x46, 0xEE},
    {0x47, 0x0d},

    // Registers of Page0
    {0xfe, 0x00}, // set page0
    {0x10, 0x26},
    {0x11, 0x0d}, // fd,modified by mormo 2010/07/06
    {0x1a, 0x2a}, // 1e,modified by mormo 2010/07/06

    {0x1c, 0x49}, // c1,modified by mormo 2010/07/06
    {0x1d, 0x9a}, // 08,modified by mormo 2010/07/06
    {0x1e, 0x61}, // 60,modified by mormo 2010/07/06

    {0x3a, 0x20},

    {0x50, 0x14}, // 10,modified by mormo 2010/07/06
    {0x53, 0x80},
    {0x56, 0x80},

    {0x8b, 0x20}, // LSC
    {0x8c, 0x20},
    {0x8d, 0x20},
    {0x8e, 0x14},
    {0x8f, 0x10},
    {0x90, 0x14},

    {0x94, 0x02},
    {0x95, 0x07},
    {0x96, 0xe0},

    {0xb1, 0x40}, // YCPT
    {0xb2, 0x40},
    {0xb3, 0x48},
    {0xb6, 0xe0},

    {0xd0, 0xc9}, // AECT  c9,modifed by mormo 2010/07/06
    {0xd3, 0x68}, // 80,modified by mormor 2010/07/06
    {0xf2, 0x02},
    {0xf7, 0x12},
    {0xf8, 0x0a},

    // Registers of Page1
    {0xfe, 0x01}, // set page1
    {0x02, 0x20},
    {0x04, 0x10},
    {0x05, 0x08},
    {0x06, 0x20},
    {0x08, 0x0a},

    {0x0e, 0x44},
    {0x0f, 0x32},
    {0x10, 0x41},
    {0x11, 0x37},
    {0x12, 0x22},
    {0x13, 0x19},
    {0x14, 0x44},
    {0x15, 0x44},

    {0x19, 0x50},
    {0x1a, 0xd8},

    {0x32, 0x10},

    {0x35, 0x00},
    {0x36, 0x80},
    {0x37, 0x00},
    //-----------Update the registers end---------//

    {0xfe, 0x00}, // set page0
    //{0xd2, 0x90}, // open AEC

    //-----------GAMMA Select(3)---------------//
    {0x9F, 0x10},
    {0xA0, 0x20},
    {0xA1, 0x38},
    {0xA2, 0x4E},
    {0xA3, 0x63},
    {0xA4, 0x76},
    {0xA5, 0x87},
    {0xA6, 0xA2},
    {0xA7, 0xB8},
    {0xA8, 0xCA},
    {0xA9, 0xD8},
    {0xAA, 0xE3},
    {0xAB, 0xEB},
    {0xAC, 0xF0},
    {0xAD, 0xF8},
    {0xAE, 0xFD},
    {0xAF, 0xFF},

    {0x14, 0x11},
    //{0x25, 0x0f}, // enable output
    ENDMARKER,
};
#else
static struct regval_list gc0308_init_regs[] = {
    {0xfe,0x80},
    {0xfe,0x00},   // set page0
    {0xd2,0x10},   // close AEC
    {0x22,0x55},   // close AWB
    {0x03,0x01},
    {0x04,0x2c},
    {0x01,0x6a},
    {0x02,0x20},
    {0x0f,0x00},
    {0x05,0x00}, //window
    {0x06,0x00},
    {0x07,0x00},
    {0x08,0x00},
    {0x09,0x01},
    {0x0a,0xe8},
    {0x0b,0x02},
    {0x0c,0x88},
    {0x46,0x80}, //crop
    {0x47,0x00},
    {0x48,0x00},
    {0x49,0x01},
    {0x4a,0xe0},
    {0x4b,0x02},
    {0x4c,0x80},
    {0x0d,0x02},
    {0x0e,0x02},
    {0x10,0x22},
    {0x11,0xfd},
    {0x12,0x2a},
    {0x13,0x00},
    {0x14,0x10},
    {0x15,0x0a},
    {0x16,0x05},
    {0x17,0x01},
    {0x18,0x44},
    {0x19,0x44},
    {0x1a,0x1e},
    {0x1b,0x00},
    {0x1c,0xc1},
    {0x1d,0x08},
    {0x1e,0x60},
    {0x1f,0x16},
    {0x20,0x00},
    {0x21,0x00},
    {0x22,0x00},
    {0x25,0x0f},
    {0x26,0x03},
    {0x30,0xf7},
    {0x31,0x50},
    {0x32,0x00},
    {0x39,0x04},
    {0x3a,0x18},
    {0x3b,0x20},
    {0x3c,0x00},
    {0x3d,0x00},
    {0x3e,0x00},
    {0x3f,0x00},
    {0x50,0x10},
    {0x53,0x80},
    {0x54,0x80},
    {0x55,0x80},
    {0x56,0x80},
    {0xfe,0x00}, // set page0
    {0x10,0x26},
    {0x11,0x0d},  // fd,modified by mormo 2010/07/06
    {0x1a,0x2a},  // 1e,modified by mormo 2010/07/06
    {0x1c,0x49}, // c1,modified by mormo 2010/07/06
    {0x1d,0x9a}, // 08,modified by mormo 2010/07/06
    {0x1e,0x61}, // 60,modified by mormo 2010/07/06
    {0x3a,0x20},
    {0x50,0x14},  // 10,modified by mormo 2010/07/06
    {0x53,0x80},
    {0x56,0x80},
    {0x14,0x10},
    {0x29,0x83},// output rawdata
    {0x24,0xb8},
    ENDMARKER,
};
#endif

static struct regval_list gc0308_vga_regs[] = {
    {0xfe, 0x00},
    {0x46, 0x80}, {0x47, 0x00},
    {0x48, 0x00}, {0x49, 0x01},
    {0x4a, 0xE0}, {0x4b, 0x02},
    {0x4c, 0x80},

    {0xfe, 0x01},
    {0x54, 0x11}, {0x55, 0x00},
    {0x56, 0x00}, {0x57, 0x00},
    {0x58, 0x00}, {0x59, 0x00},
    ENDMARKER,
};

static struct regval_list gc0308_regs_stream_on[] = {
    {0xfe, 0x00},
    {0x25, 0x0f},
    ENDMARKER,
};

static struct regval_list gc0308_regs_stream_off[] = {
    {0xfe, 0x00},
    {0x25, 0x00},
    ENDMARKER,
};

static struct regval_list gc0308_chip_id_regs[] = {
    {0xfe, 0x00},
    {0x00, 0x00},
    ENDMARKER,
};

static struct regval_list gc0308_regs_exp_t[] = {
    {0xfe, 0x00},
    {0x03, 0x01},
    {0x04, 0x2c},
    ENDMARKER,
};

static struct regval_list gc0308_regs_gain[] = {
    {0xfe, 0x00},
    {0x50, 0x10},
    {0x51, 0x40},
    {0x52, 0x40},
    ENDMARKER,
};

static int gc0308_read(struct i2c_device *i2c, unsigned char reg, unsigned char *value)
{
    struct i2c_msg msg[2] = {
        [0] = {
            .flags  = 0,
            .len    = 1,
            .buf    = &reg,
        },
        [1] = {
            .flags  = I2C_M_RD,
            .len    = 1,
            .buf    = value,
        }
    };

    int ret = i2c_transfer(i2c, msg, 2);
    if (ret > 0)
        ret = 0;

    return ret;
}

static int gc0308_write(struct i2c_device *i2c, unsigned char reg, unsigned char value)
{
    unsigned char buf[2] = {reg, value};
    struct i2c_msg msg = {
        .flags = 0,
        .len = 2,
        .buf = buf,
    };

    int ret = i2c_transfer(i2c, &msg, 1);
    if (ret > 0)
        ret = 0;

    return ret;
}

static inline int gc0308_read_array(struct i2c_device *i2c, struct regval_list *vals)
{
    int ret;

    while ((vals->reg_num != 0xff) || (vals->value != 0xff)) {
        if (vals->reg_num == REG_PAGE) {
            ret = gc0308_write(i2c, vals->reg_num, vals->value);
            if (ret < 0) {
                return ret;
            }
        } else {
            gc0308_read(i2c, vals->reg_num, &vals->value);
        }
        vals++;
    }

    return 0;
}

static int gc0308_write_array(struct i2c_device *i2c, struct regval_list *vals)
{
    int ret;

    while ((vals->reg_num != 0xff) || (vals->value != 0xff)) {
        ret = gc0308_write(i2c, vals->reg_num, vals->value);
        if (ret < 0) {
            return ret;
            printf("write error\n");
        }
        vals++;
    }

    // while (vals->reg_num != GC0308_FLAG_END) {
    //     if (vals->reg_num == GC0308_FLAG_DELAY) {
    //             msleep(vals->value);
    //     } else {
    //         ret = gc0308_write(i2c, vals->reg_num, vals->value);
    //         if (ret < 0)
    //             return ret;
    //     }
    //     vals++;
    // }

    return 0;
}

static int is_inited = 0;

static int init_res(int cim_id)
{
    int ret;
    char gpio_str[10];

    if (is_inited)
        return 0;

    ret = dvp_init_select_gpio(cim_id);
    if (ret) {
        printf("gc0308: failed to init dvp pins\n");
        return ret;
    }

    if (pwdn_gpio != -1) {
        ret = gpio_request(pwdn_gpio, "gc0308_pwdn");
        if (ret) {
            printf("gc0308: failed to request pwdn pin: %s\n", gpio_to_str(pwdn_gpio, gpio_str, sizeof(gpio_str)));
            goto err_pwdn_gpio;
        }
    }

    if (reset_gpio != -1) {
        ret = gpio_request(reset_gpio, "gc0308_reset");
        if (ret) {
            printf("gc0308: failed to request reset pin: %s\n", gpio_to_str(reset_gpio, gpio_str, sizeof(gpio_str)));
            goto err_reset_gpio;
        }
    }

    if (power_gpio != -1) {
#ifdef CONFIG_RUN_ON_FPGA
#else
        ret = gpio_request(power_gpio, "gc0308_power");
        if (ret) {
            printf("gc0308: failed to request power pin: %s\n", gpio_to_str(power_gpio, gpio_str, sizeof(gpio_str)));
            goto err_power_gpio;
        }
#endif
    }

    i2c_dev = i2c_register(bus_num, I2C_ADDR, I2C_ADDR_BIT_7, "gc0308_i2c");
    if (i2c_dev == NULL) {
        printf("gc0308: failed to request i2c bus: %d\n", bus_num);
        goto err_i2c_register;
    }

    is_inited = 1;

    return 0;

err_i2c_register:
    if (power_gpio != -1)
        gpio_release(power_gpio);
err_power_gpio:
    if (reset_gpio != -1)
        gpio_release(reset_gpio);
err_reset_gpio:
    if (pwdn_gpio != -1)
        gpio_release(pwdn_gpio);
err_pwdn_gpio:
    dvp_deinit_gpio(cim_id);

    return ret;
}

static void deinit_res(int cim_id)
{
    if (!is_inited)
        return;

    is_inited = 0;

    i2c_unregister(i2c_dev);

    if (reset_gpio != -1)
        gpio_release(reset_gpio);

    if (pwdn_gpio != -1)
        gpio_release(pwdn_gpio);

    if (power_gpio != -1)
        gpio_release(power_gpio);

    dvp_deinit_gpio(cim_id);
}

static int gc0308_stream_on(void)
{
    int ret = gc0308_write_array(i2c_dev, gc0308_regs_stream_on);
    if (ret)
        printf("gc0308: failed to stream on\n");

    return ret;
}

static void gc0308_stream_off(void)
{
    int ret = gc0308_write_array(i2c_dev, gc0308_regs_stream_off);
    if (ret)
        printf("gc0308: failed to stream off\n");
}

static void gc0308_power_off(int cim_id)
{
    if (reset_gpio != -1)
        gpio_direction_output(reset_gpio, 0);

    if (pwdn_gpio != -1)
        gpio_direction_output(pwdn_gpio, 0);

    if (power_gpio != -1)
        gpio_direction_output(power_gpio, 0);

    camera_disable_sensor_mclk(cim_id);
}

static int gc0308_power_on(int cim_id)
{
    int ret;

    ret = init_res(cim_id);
    if (ret)
        return ret;

    camera_enable_sensor_mclk(cim_id, 24 * 1000 * 1000);

    if (power_gpio != -1) {
        gpio_direction_output(power_gpio, 1);
        m_msleep(50);
    }

    if (reset_gpio != -1) {
        gpio_direction_output(reset_gpio, 1);
        msleep(20);
        gpio_direction_output(reset_gpio, 0);
        msleep(20);
        gpio_direction_output(reset_gpio, 1);
        /* Match the proven GC0308 reset sequence before the first I2C read. */
        msleep(10);
    }

    if (pwdn_gpio != -1) {
        gpio_direction_output(pwdn_gpio, 1);
        msleep(10);
        gpio_direction_output(pwdn_gpio, 0);
        msleep(10);
    }

    /*
     * check sensor product ID
     */
    ret = gc0308_read_array(i2c_dev, gc0308_chip_id_regs);
    if (ret < 0) {
        printf("gc0308: Failed to read chip id\n");
        goto err;
    }

    if (gc0308_chip_id_regs[1].value != GC0308_CHIP_ID) {
        printf("gc0308: read sensor chip_id is error: %x\n", (int)gc0308_chip_id_regs[1].value);
        ret = -ENODEV;
        goto err;
    }

    printf("find sensor gc0308 id=0x%x\n", gc0308_chip_id_regs[1].value);

    ret = gc0308_write_array(i2c_dev, gc0308_init_regs);
    // ret = gc0308_write_array(i2c_dev, gc0308_init_regs_raw);
    if (ret) {
        printf("gc0308: failed to init setting\n");
        goto err;
    }

    /*
     *  Set resolution vga(640*480)
     */
    ret = gc0308_write_array(i2c_dev, gc0308_vga_regs);
    if (ret < 0) {
        printf("gc0308: failed to write window size %d", ret);
        goto err;
    }

    return 0;

err:
    gc0308_power_off(cim_id);
    deinit_res(cim_id);
    return ret;
}

static int gc0308_g_register(struct sensor_dbg_register *reg)
{
    int ret;
    unsigned char val;

    ret = gc0308_read(i2c_dev, reg->reg & 0xff, &val);
    if (ret < 0) {
        printf("gc0308: get register failed\n");
        return ret;
    }
    printf("gc0308: get register %u\n", val);

    reg->val = val;
    reg->size = 2;

    return 0;
}

static int gc0308_s_register(struct sensor_dbg_register *reg)
{
    return gc0308_write(i2c_dev, reg->reg & 0xff, reg->val & 0xff);
}

static int set_exp_t(unsigned int exp)
{
    int ret = 0;
    if (exp > VTS) exp = VTS;
    gc0308_regs_exp_t[1].value = ((exp >> 8) & 0x0f);
    gc0308_regs_exp_t[2].value = (exp & 0xff);
    ret = gc0308_write_array(i2c_dev, gc0308_regs_exp_t);
    if (ret < 0) {
        return ret;
    }
    return ret;
}

static int set_gain(unsigned int gain)
{
    int ret = 0;
    unsigned int global_gain = 0x10;
    unsigned int pre_gain = 0x40;
    unsigned int post_gain = 0x40;
    if (gain < 1024 * 4 ) {
        global_gain = gain * 16 / 1024;
    } else if(gain < 1024 * 16) {
        global_gain = 0x3f;
        pre_gain = gain * 64 / 4 / 1024;

    } else if(gain < 1024 * 64) {
        global_gain = 0x3f;
        pre_gain = 0xff;
        post_gain = gain * 16 * 64 / 16 / 1024;
    } else {
        global_gain = 0x3f;
        pre_gain = 0xff;
        post_gain = 0xff;
    }

    gc0308_regs_gain[1].value = global_gain;
    gc0308_regs_gain[2].value = pre_gain;
    gc0308_regs_gain[3].value = post_gain;
    ret = gc0308_write_array(i2c_dev, gc0308_regs_gain);
    if (ret < 0) {
        return ret;
    }
    return ret;
}

static int set_ae_reg(unsigned *_ae_reg)
{
    int ret = 0;
    ret = set_exp_t(*(_ae_reg));
    if (ret < 0) {
        return ret;
    }
    ret = set_gain(*(_ae_reg + 1));
    if (ret < 0) {
        return ret;
    }
    return ret;
}

static int get_reg(struct regval_list reg)
{

    int ret = 0;
    unsigned char val;
    ret = gc0308_read(i2c_dev, reg.reg_num & 0xff, &val);
    if (ret < 0) {
        return ret;
    }
    printf("gc0308(%x):  read reg: %x\n", reg.reg_num, val);
    return ret;
}

static int get_ae_reg(void)
{
    int ret = 0;
    ret = get_reg(gc0308_regs_exp_t[1]);
    if (ret < 0)
        return ret;
    ret = get_reg(gc0308_regs_exp_t[2]);
    if (ret < 0)
        return ret;
    ret = get_reg(gc0308_regs_gain[1]);
    if (ret < 0)
        return ret;
    ret = get_reg(gc0308_regs_gain[2]);
    if (ret < 0)
        return ret;
    ret = get_reg(gc0308_regs_gain[3]);
    if (ret < 0)
        return ret;
    return ret;
}

static int gc0308_s_ae_register(unsigned *ae_reg)
{
    return set_ae_reg(ae_reg);
}

static int gc0308_g_ae_register(void)
{
    return get_ae_reg();
}

static int gc0308_set_fps(unsigned int fps)
{
    unsigned int total = VTS * FPS;
    int vb = total / fps - GC0308_HEIGHT;
    if (fps * GC0308_HEIGHT > total) {
        printf("gc0308: set fps failed,fps is too high\n");
        return -1;
    }

    gc0308_write(i2c_dev, GC0308_PAGE_REG, (unsigned char)(0x00));
    gc0308_write(i2c_dev, GC0308_VB_H, (unsigned char)((vb & 0xf00) >> 4));
    gc0308_write(i2c_dev, GC0308_VB_L, (unsigned char)(vb & 0xff));

    return 0;
}

static int gc0308_get_fps(void)
{
    unsigned int total = VTS * FPS;
    unsigned char vb_h,vb_l;
    unsigned int vb = 0;

    gc0308_write(i2c_dev, GC0308_PAGE_REG, (unsigned char)(0x00));
    gc0308_read(i2c_dev, GC0308_VB_H, &vb_h);
    gc0308_read(i2c_dev, GC0308_VB_L, &vb_l);

    vb = vb_h << 4 | vb_l;
    return total / (vb + GC0308_HEIGHT);
}

static int gc0308_set_gain(unsigned int gain)
{
    unsigned char global_gain = 0x10;
    unsigned char pre_gain = 0x40;
    unsigned char post_gain = 0x40;

    if (gain > 64 * 1024) {
        printf("gc0308: set gain failed,gain is set 64\n");
        global_gain = 0x3f;
        pre_gain = 0xff;
        post_gain = 0xff;
    } else if (gain > 16 * 1024) {
        global_gain = 0x3f;
        pre_gain = 0xff;
        post_gain = gain * 64 / 16 / 1024;
    } else if (gain > 4 * 1024) {
        global_gain = 0x3f;
        pre_gain = gain * 64 / 4 / 1024;
    } else if (gain > 0) {
        global_gain = gain * 16 / 1024;
    } else {
        printf("gc0308: set gain failed,gain is set 0\n");
        global_gain = 0x00;
    }
    printf("global_gain:%x pre_gain:%x post_gain:%x\n",global_gain,pre_gain,post_gain);
    gc0308_write(i2c_dev, GC0308_PAGE_REG, (unsigned char)(0x00));
    gc0308_write(i2c_dev, GC0308_GLOBAL_GAIN, global_gain);
    gc0308_write(i2c_dev, GC0308_PREGAIN, pre_gain);
    gc0308_write(i2c_dev, GC0308_POSTGAIN, post_gain);

    return 0;
}

static int gc0308_get_gain(void)
{
    unsigned char global_gain = 0x10;
    unsigned char pre_gain = 0x40;
    unsigned char post_gain = 0x40;
    int gain = 0;

    gc0308_write(i2c_dev, GC0308_PAGE_REG, (unsigned char)(0x00));
    gc0308_read(i2c_dev, GC0308_GLOBAL_GAIN, &global_gain);
    gc0308_read(i2c_dev, GC0308_PREGAIN, &pre_gain);
    gc0308_read(i2c_dev, GC0308_POSTGAIN, &post_gain);
    if (global_gain == 0x3f) {
        gain = 4 * pre_gain * post_gain /64 /64;
    }
    gain = ((global_gain) * (pre_gain) * (post_gain)) /64 /64;
    return gain;
}

static int gc0308_set_exp(unsigned int exp)
{
    if (exp > 0xfff) {
        printf("gc0308: set exp failed,exp is set 0xfff\n");
        exp = 0xfff;
    } else if (exp == 0U) {
        printf("gc0308: set exp failed,exp is set 1\n");
        exp = 1U;
    }

    gc0308_write(i2c_dev, GC0308_PAGE_REG, (unsigned char)(0x00));
    gc0308_write(i2c_dev, GC0308_EXP_H, (unsigned char)((exp & 0xf00) >> 8));
    gc0308_write(i2c_dev, GC0308_EXP_L, (unsigned char)(exp & 0xff));

    return 0;
}

static int gc0308_get_exp(void)
{
    unsigned char exp_h, exp_l;
    unsigned int exp = 0;

    gc0308_write(i2c_dev, GC0308_PAGE_REG, (unsigned char)(0x00));
    gc0308_read(i2c_dev, GC0308_EXP_H, &exp_h);
    gc0308_read(i2c_dev, GC0308_EXP_L, &exp_l);

    exp = exp_h << 8 | exp_l;
    printf("exp:%d\n",exp);
    printf("exp_h:%x exp_l:%x\n",exp_h, exp_l);
    return exp;
}

static struct sensor_attr gc0308_sensor_config = {
    .device_name        = "gc0308",

    .dma_mode           = SENSOR_DATA_DMA_MODE_YUV420, /* 是否单独提取Y数据 */
    .dbus_type          = SENSOR_DATA_BUS_DVP,
    .dvp = {
        .hsync_polarity = POLARITY_HIGH_ACTIVE,
        .vsync_polarity = POLARITY_HIGH_ACTIVE,
        .pclk_polarity  = POLARITY_SAMPLE_RISING,
        .img_scan_mode  = DVP_IMG_SCAN_PROGRESS,
    },

    .sensor_info = {
        .width          = GC0308_WIDTH,
        .height         = GC0308_HEIGHT,
        // .fmt            = SENSOR_PIXEL_FMT_Y8_1X8,
        .fmt            = SENSOR_PIXEL_FMT_YUYV8_2X8,
        // .fmt            = SENSOR_PIXEL_FMT_SRGGB8_1X8,
        .fps            = 30 << 16 | 1,
    },

    .ops = {
        .power_on        = gc0308_power_on,
        .power_off       = gc0308_power_off,
        .stream_on       = gc0308_stream_on,
        .stream_off      = gc0308_stream_off,
        .get_register    = gc0308_g_register,
        .set_register    = gc0308_s_register,
        .get_ae_register = gc0308_g_ae_register,
        .set_ae_register = gc0308_s_ae_register,
        .set_fps         = gc0308_set_fps,
        .get_fps         = gc0308_get_fps,
        .set_gain        = gc0308_set_gain,
        .get_gain        = gc0308_get_gain,
        .set_exp         = gc0308_set_exp,
        .get_exp         = gc0308_get_exp,
    },
};

void gc0308_dvp_sensor_init(int cim_id)
{
    camera_register_sensor(cim_id, &gc0308_sensor_config);
}
