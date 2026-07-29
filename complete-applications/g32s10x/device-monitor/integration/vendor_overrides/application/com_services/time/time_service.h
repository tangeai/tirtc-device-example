#ifndef __TIME_SERVICE_H__
#define __TIME_SERVICE_H__
#include <driver/rtc.h>

#define JZ_NTP_SERVER "ntp.aliyun.com"
#define JZ_TIME_ZONE "Etc/GMT-8"
#define JZ_NTP_PORT 123
#define JZ_NTP_PACKET_SIZE 48
#define JZ_NTP_DELTA 2208988800UL // 1900-1970的秒数差
#define JZ_NTP_RETRY_INITIAL_US (5U * 1000U * 1000U)
#define JZ_NTP_RETRY_MAX_US (60U * 1000U * 1000U)
#define JZ_NTP_FAIL_SYNC_PERIOD_US (30U * 60U * 1000U * 1000U)
#define JZ_NTP_SUCCESS_SYNC_PERIOD_S (24U * 60U * 60U)

typedef struct {
    uint8_t li_vn_mode; /* 0-1: leap, 2-4: version, 5-7: mode */
    uint8_t stratum;
    uint8_t poll;
    uint8_t precision;
    uint32_t rootDelay;
    uint32_t rootDispersion;
    uint32_t refId;
    uint32_t refTm_s;
    uint32_t refTm_f;
    uint32_t origTm_s;
    uint32_t origTm_f;
    uint32_t rxTm_s;
    uint32_t rxTm_f;
    uint32_t txTm_s; /* 服务器发送时间 */
    uint32_t txTm_f;
} jz_ntp_packet_t;

typedef struct rtc_time ui_rtc_time;
/**
 * @brief APP_COMMON接口-设置时间
 * @param hour  时
 * @param min   分
 */
void jz_set_rtc_time(uint8_t hour, uint8_t min);

/**
 * @brief APP_COMMON接口-设置日期
 * @param year  年
 * @param mon   月
 * @param mday  日
 */
void jz_set_rtc_date(uint16_t year, uint8_t mon, uint8_t mday);

/**
 * @brief 获取NTP服务器的时间
 * @param ntp_server_ip NTP服务器地址
 * @param time_zone 时区
 * @return -1-失败 0-成功
 */
int jz_set_ntp_server(const char *server);

/**
 * @brief 设置时区
 * @param tz 时区
 * @return -1-失败 0-成功
 */
int jz_set_time_zone(const char *tz);

/**
 * @brief 获取NTP服务器地址
 */
const char *jz_get_ntp_server(void);

/**
 * @brief 获取时区
 */
const char *jz_get_time_zone(void);

/**
 * @brief 获取NTP服务器的时间
 * @return -1-失败 0-成功
 */
int jz_get_ntp_server_time(void);

/**
 * @brief time service初始化
 */
void jz_time_service_init(void);

#endif /*__TIME_SERVICE_H__*/
