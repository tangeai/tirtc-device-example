#include <stdio.h>
#include <driver/systick.h>
#include <cpu/spinlock.h>
#include <common.h>
#include "xformatc.h"
#include <module.h>
#include "printf.h"
#include <libgen.h>

static int printf_time = 1;

static int printf_no_time = 0;

int g_log_level[JZ_LOG_TOTAL_NUM] = {
    JZ_LOG_DUMP, /* com module */
    JZ_LOG_DUMP, /* msc module */
    JZ_LOG_DUMP, /* spi module */
    JZ_LOG_DUMP, /* aic module */
    JZ_LOG_DUMP, /* pwm module */
    JZ_LOG_DUMP, /* I2D module */
    JZ_LOG_DUMP, /* dtrng module */
    JZ_LOG_DUMP, /* adc module */
    JZ_LOG_DUMP, /* tcu module */
    JZ_LOG_DUMP, /* jpeg module */
    JZ_LOG_DUMP, /* msca module */
    JZ_LOG_DUMP, /* cim module */
    JZ_LOG_DUMP, /* gpio module */
    JZ_LOG_DUMP, /* dma module */
    JZ_LOG_DUMP, /* net module */
    JZ_LOG_DUMP, /* wdt module */
    JZ_LOG_INFO, /* rtc module */
    JZ_LOG_DUMP, /* rsa module */
    JZ_LOG_DUMP, /* efuse module */
    JZ_LOG_DUMP, /* ui module */
    JZ_LOG_DUMP, /* app common module */
    JZ_LOG_DUMP, /* upgrade module */
};

char g_log_mod_name[][16] = {"COM", "MSC", "SPI", "AIC", "PWM", "I2D", "DTRNG", "ADC", "TCU", "JPEG", "MSCA", "CIM", "GPIO", "DMA", "NET", "WDT", "RTC", "RSA", "EFUSE", "UI", "APP_COM", "UPGRADE"};

char g_log_level_name[][12] = {"ERR", "DUMP", "WARN", "INFO", "DBG"};

extern int __io_putchar(int ch);

void printf_enable_time_stamp(void)
{
    printf_no_time = 0;
}

void printf_disable_time_stamp(void)
{
    printf_no_time = 1;
}

static void m_printf_putchar(void *arg, char c)
{
    (void)arg;

    if (c == '\n')
        printf_time = 1;

    __io_putchar(c);
}

static DEFINE_SPINLOCK(printf_lock);
int log_do(int level, int module, const char *file, int line, const char *pFmt, ...)
{
    char buf[MAX_LOG_LINE_LENGTH] = {0};
    int len = 0;
    int maxLen = MAX_LOG_LINE_LENGTH;
    va_list list;
    unsigned long flags;

    if (level > g_log_level[module])
        return 0;

    const char *filename = strrchr(file, '/');
    if (filename == NULL) {
        filename = file;
    } else {
        filename++;
    }

    if (printf_time && !printf_no_time) {
        uint64_t now = systick_get_time_us();
        printf_time = 0;
        len = snprintf(buf, sizeof(buf), "[%lld.%06lld] ", now / USEC_PER_SEC, now % USEC_PER_SEC);
    }

    len += snprintf(buf + len, maxLen - len, "[%s][%s][%s][%d]:", g_log_mod_name[module], g_log_level_name[level],filename, line);

    spin_lock_irqsave(&printf_lock, flags);

    for (int i = 0; i < len; i++) {
        m_printf_putchar(0, buf[i]);
    }

    va_start(list, pFmt);
    (void)xvformat(m_printf_putchar, 0, pFmt, list);
    va_end(list);

    spin_unlock_irqrestore(&printf_lock, flags);

    return 0;
}

int printf(const char *__restrict fmt, ...)
{
    va_list list;
    unsigned int count;
    unsigned long flags;

    spin_lock_irqsave(&printf_lock, flags);

    if (printf_time && !printf_no_time) {
        uint64_t now = systick_get_time_us();
        printf_time = 0;
        // 格式化时间戳并直接输出
        char time_buf[32]; // 假设时间戳字符串不会超过32个字符
        int time_len = snprintf(time_buf, sizeof(time_buf), "[%lld.%06lld] ", now / USEC_PER_SEC, now % USEC_PER_SEC);

        // 使用 m_printf_putchar 输出时间戳
        for (int i = 0; i < time_len; i++) {
            m_printf_putchar(0, time_buf[i]);
        }
    }

    va_start(list, fmt);
    count = xvformat(m_printf_putchar, 0, fmt, list);
    va_end(list);

    spin_unlock_irqrestore(&printf_lock, flags);

    return count;
}
EXPORT_SYMBOL(printf);

static void m_sprintf_putchar(void *arg, char c)
{
    char *sp = *(char **)arg;

    *sp = c;
    (*(char **)arg)++;
}

int sprintf(char *__restrict s, const char *__restrict fmt, ...)
{
    va_list list;
    unsigned count;

    assert(s);
    assert(fmt);

    va_start(list, fmt);
    count = xvformat(m_sprintf_putchar, s, fmt, list);
    va_end(list);

    s[count] = 0;

    return count;
}

struct snprintf_t {
    size_t n;
    size_t index;
    char *s;
};

static void m_snprintf_putchar(void *arg, char c)
{
    struct snprintf_t *sp = *(struct snprintf_t **)arg;

    if (sp->index >= sp->n)
        return;

    sp->index++;

    *(sp->s++) = c;
}

int snprintf(char *__restrict s, size_t n, const char *__restrict fmt, ...)
{
    va_list list;
    unsigned count;
    struct snprintf_t sp;

    sp.s = s;
    sp.n = n;
    sp.index = 0;

    assert(s);
    assert(fmt);

    va_start(list, fmt);
    count = xvformat(m_snprintf_putchar, &sp, fmt, list);
    va_end(list);

    s[count] = 0;

    return count;
}

int vsnprintf(char *__restrict s, size_t n, const char *__restrict fmt, va_list ap)
{
    unsigned count;
    struct snprintf_t sp;

    sp.s = s;
    sp.n = n;
    sp.index = 0;

    assert(s);
    assert(fmt);

    count = xvformat(m_snprintf_putchar, &sp, fmt, ap);

    s[count] = 0;

    return count;
}
