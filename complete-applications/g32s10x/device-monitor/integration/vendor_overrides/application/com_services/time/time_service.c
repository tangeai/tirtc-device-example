#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include "os/mutex.h"
#include "os/thread.h"
#include "printf.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "hmi_service.h"
#include "time_service.h"
#include "ui_time_cfg.h"
#include "wireless.h"
#include "ui_status_bar.h"
#include "jz_lvgl_mutex.h"

static jz_time_config_t *time_cfg = NULL;
static struct mutex g_time_sync_lock;

#ifdef CONFIG_TIRTC
extern long _tg_timezone_;
#endif

static int jz_parse_time_zone_offset(const char *tz, int32_t *offset_seconds)
{
    const char *cursor;
    int sign;
    int hours = 0;

    if (tz == NULL || offset_seconds == NULL || strncmp(tz, "UTC", 3) != 0) {
        return -1;
    }
    cursor = tz + 3;
    if (*cursor == '+') {
        sign = 1;
    } else if (*cursor == '-') {
        sign = -1;
    } else {
        return -1;
    }
    cursor++;
    if (*cursor < '0' || *cursor > '9') {
        return -1;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        hours = hours * 10 + (*cursor - '0');
        if (hours > 14) {
            return -1;
        }
        cursor++;
    }
    if (*cursor != '\0' || (sign < 0 && hours > 12)) {
        return -1;
    }
    *offset_seconds = (int32_t)(sign * hours * 60 * 60);
    return 0;
}

static int jz_apply_time_zone(const char *tz)
{
    char posix_tz[MAX_TIME_ZONE_LEN] = {0};
    int32_t offset_seconds;

    if (jz_parse_time_zone_offset(tz, &offset_seconds) != 0) {
        jz_log_warning(JZ_APP_COM_MOD,
                       "invalid time zone, use default UTC+8\n");
        offset_seconds = 8 * 60 * 60;
        strncpy(posix_tz, JZ_TIME_ZONE, sizeof(posix_tz) - 1);
    } else if (offset_seconds >= 0) {
        snprintf(posix_tz, sizeof(posix_tz), "Etc/GMT-%ld",
                 (long)(offset_seconds / 3600));
    } else {
        snprintf(posix_tz, sizeof(posix_tz), "Etc/GMT+%ld",
                 (long)(-offset_seconds / 3600));
    }

    if (setenv("TZ", posix_tz, 1) != 0) {
        jz_log_error(JZ_APP_COM_MOD, "set time zone environment failed\n");
        return -1;
    }
    tzset();
#ifdef CONFIG_TIRTC
    _tg_timezone_ = (long)offset_seconds;
#endif
    jz_log_info(JZ_APP_COM_MOD,
                "time zone applied config=%s posix=%s offset=%ld\n",
                tz != NULL ? tz : "invalid", posix_tz,
                (long)offset_seconds);
    return 0;
}

/* 检查字符串是否为IP地址 */
static int is_ip_address(const char *str)
{
    struct in_addr addr;
    return inet_pton(AF_INET, str, &addr) == 1;
}

static void create_status_bar_time_task(void)
{
    /* 创建定时器任务，每分钟更新一次*/
    lv_timer_create(jz_update_time, 60000, NULL);
}

static void jz_refresh_status_bar_time_safe(void)
{
    jz_lv_mutex_lock();
    jz_update_time(NULL);
    jz_lv_mutex_unlock();
}

static void jz_time_sync_service_handler(const jz_hmi_msg_t *msg)
{
    (void)msg;

    if (jz_get_ntp_server_time() == 0) {
        jz_refresh_status_bar_time_safe();
    }
}

void jz_set_rtc_time(uint8_t hour, uint8_t min)
{
#ifdef CONFIG_RTC
    struct rtc_time rtc;
    unsigned long long time;

    rtc_get_current_tm(&rtc);

    rtc.tm_hour = hour;
    rtc.tm_min = min;

    rtc_tm_to_time(&rtc, &time);

    rtc_set_time(time);
    jz_update_time(NULL);
#else
    jz_log_info(JZ_APP_COM_MOD, "RTC is not configured!\n");
#endif
}

void jz_set_rtc_date(uint16_t year, uint8_t mon, uint8_t mday)
{
#ifdef CONFIG_RTC
    struct rtc_time rtc;
    unsigned long long time;

    rtc_get_current_tm(&rtc);

    rtc.tm_year = year;
    rtc.tm_mon = mon;
    rtc.tm_mday = mday;

    rtc_tm_to_time(&rtc, &time);

    rtc_set_time(time);
    jz_update_time(NULL);
#else
    jz_log_info(JZ_APP_COM_MOD, "RTC is not configured!\n");
#endif
}

int jz_set_ntp_server(const char *server) {
    if (server == NULL) {
        jz_log_error(JZ_APP_COM_MOD, "ntp server address is null\n");
        return -1;
    }

    size_t len = strlen(server);
    if (len >= MAX_NTP_SERVER_LEN) {
        jz_log_error(JZ_APP_COM_MOD, "ntp server address length more than 255\n");
        return -1;
    }

    /* 保存用户数据 */
    time_cfg = jz_get_time_config();
    strncpy(time_cfg->server_ip, server, MAX_NTP_SERVER_LEN - 1);
    time_cfg->server_ip[MAX_NTP_SERVER_LEN - 1] = '\0';
    jz_log_info(JZ_APP_COM_MOD, "ntp server ip %s\n", time_cfg->server_ip);
    if (jz_save_time_config(time_cfg)) {
        jz_log_error(JZ_APP_COM_MOD, "save time config failed\n");
        return -1;
    }
    return 0;
}

int jz_set_time_zone(const char *tz) {
    int32_t offset_seconds;

    if (jz_parse_time_zone_offset(tz, &offset_seconds) != 0) {
        (void)jz_apply_time_zone(NULL);
        return -1;
    }
    (void)offset_seconds;

    /* 保存用户数据 */
    time_cfg = jz_get_time_config();
    if (strcmp(time_cfg->time_zone, tz) != 0) {
        strncpy(time_cfg->time_zone, tz, MAX_TIME_ZONE_LEN - 1);
        time_cfg->time_zone[MAX_TIME_ZONE_LEN - 1] = '\0';
        jz_log_info(JZ_APP_COM_MOD, "user set time zone %s\n",
                    time_cfg->time_zone);
        if (jz_save_time_config(time_cfg)) {
            jz_log_error(JZ_APP_COM_MOD, "save time config failed\n");
            return -1;
        }
    }
    return jz_apply_time_zone(tz);
}

static int jz_resolve_ntp_server(const char *server,
                                 struct sockaddr_in *serv_addr)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct sockaddr_in *ipv4;
    char dns_server[IPADDR_STRLEN_MAX] = "unknown";
    int dns_result;

    if (is_ip_address(server)) {
        if (inet_pton(AF_INET, server, &serv_addr->sin_addr) != 1) {
            return -1;
        }
        return 0;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    dns_result = getaddrinfo(server, NULL, &hints, &addresses);
    if (dns_result != 0 || addresses == NULL) {
        (void)ipaddr_ntoa_r(dns_getserver(0), dns_server,
                            sizeof(dns_server));
        jz_log_error(JZ_APP_COM_MOD,
                     "NTP DNS failed server=%s result=%d dns=%s\n",
                     server, dns_result, dns_server);
        if (addresses != NULL) {
            freeaddrinfo(addresses);
        }
        return -1;
    }
    if (addresses->ai_addr == NULL ||
        addresses->ai_addrlen < sizeof(struct sockaddr_in)) {
        freeaddrinfo(addresses);
        jz_log_error(JZ_APP_COM_MOD, "NTP DNS returned invalid address\n");
        return -1;
    }
    ipv4 = (struct sockaddr_in *)addresses->ai_addr;
    serv_addr->sin_addr = ipv4->sin_addr;
    freeaddrinfo(addresses);
    return 0;
}

int jz_get_ntp_server_time(void)
{
    int sockfd = -1;
    int ret = -1;
    uint32_t ntp_time = 0;
    uint64_t unix_seconds;
    int64_t local_seconds;
    int32_t timezone_offset;
    struct sockaddr_in serv_addr;
    struct rtc_time local_tm;
    struct rtc_time rtc_value;
    jz_ntp_packet_t packet;
    char ntp_server[MAX_NTP_SERVER_LEN];
    char time_zone[MAX_TIME_ZONE_LEN];
    int bytes_sent, bytes_received;
    struct timeval tv;
    socklen_t addr_len;

    if (!mutex_try_lock(&g_time_sync_lock)) {
        jz_log_warning(JZ_APP_COM_MOD, "time sync is busy\n");
        return -1;
    }

    time_cfg = jz_get_time_config();
    if (!time_cfg->server_ip[0]|| !time_cfg->time_zone[0]) {
        jz_log_error(JZ_APP_COM_MOD, "Param invalid\n");
        goto out;
    }
    strncpy(ntp_server, time_cfg->server_ip, sizeof(ntp_server) - 1);
    ntp_server[sizeof(ntp_server) - 1] = '\0';
    strncpy(time_zone, time_cfg->time_zone, sizeof(time_zone) - 1);
    time_zone[sizeof(time_zone) - 1] = '\0';
    if (jz_parse_time_zone_offset(time_zone, &timezone_offset) != 0) {
        jz_log_error(JZ_APP_COM_MOD, "invalid time zone: %s\n", time_zone);
        goto out;
    }
    jz_log_info(JZ_APP_COM_MOD, "NTP sync begin server=%s zone=%s\n",
                ntp_server, time_zone);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        jz_log_error(JZ_APP_COM_MOD, "Socket create failed\n");
        goto out;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(JZ_NTP_PORT);
    if (jz_resolve_ntp_server(ntp_server, &serv_addr) != 0) {
        goto out;
    }

    /* Bound the blocking receive before starting network I/O. */
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        jz_log_error(JZ_APP_COM_MOD, "Failed to set socket timeout\n");
        goto out;
    }

    /* 构造NTP请求包 */
    memset(&packet, 0, JZ_NTP_PACKET_SIZE);
    packet.li_vn_mode = 0x23; /* LI=0, VN=4, Mode=3 (客户端模式) */
    bytes_sent = sendto(sockfd, &packet, JZ_NTP_PACKET_SIZE, 0, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (bytes_sent != JZ_NTP_PACKET_SIZE) {
        jz_log_error(JZ_APP_COM_MOD, "Failed to send NTP request size=%d\n",
                     bytes_sent);
        goto out;
    }

    /* 接收NTP响应 */
    addr_len = sizeof(serv_addr);
    bytes_received = recvfrom(sockfd, &packet, JZ_NTP_PACKET_SIZE, 0, (struct sockaddr *)&serv_addr, &addr_len);
    if (bytes_received < 0) {
        jz_log_error(JZ_APP_COM_MOD, "Failed to receive NTP response\n");
        goto out;
    }
    if (bytes_received != JZ_NTP_PACKET_SIZE) {
        jz_log_error(JZ_APP_COM_MOD, "Invalid NTP response size=%d\n",
                     bytes_received);
        goto out;
    }
    if ((packet.li_vn_mode >> 6) == 3 ||
        (packet.li_vn_mode & 0x07) != 4 ||
        packet.stratum == 0 || packet.stratum > 15) {
        jz_log_error(JZ_APP_COM_MOD,
                     "Invalid NTP response flags=0x%02x stratum=%u\n",
                     (unsigned int)packet.li_vn_mode,
                     (unsigned int)packet.stratum);
        goto out;
    }

    /* 解析并转换时间 */
    ntp_time = ntohl(packet.txTm_s);
    if (ntp_time <= JZ_NTP_DELTA) {
        jz_log_error(JZ_APP_COM_MOD, "Invalid NTP transmit time=%lu\n",
                     (unsigned long)ntp_time);
        goto out;
    }
    unix_seconds = (uint64_t)ntp_time - JZ_NTP_DELTA;
    local_seconds = (int64_t)unix_seconds + timezone_offset;
    if (local_seconds < 0) {
        jz_log_error(JZ_APP_COM_MOD, "NTP local time underflow\n");
        goto out;
    }
    rtc_time_to_tm((unsigned long long)local_seconds, &local_tm);
    if (rtc_valid_tm(&local_tm) != 0) {
        jz_log_error(JZ_APP_COM_MOD, "NTP converted calendar is invalid\n");
        goto out;
    }
    rtc_value = local_tm;
    rtc_value.tm_year += 1900;
    rtc_value.tm_mon += 1;
#ifdef CONFIG_RTC
    /* 设置rtc时间 */
    rtc_set_tm(&rtc_value);
#endif
    jz_log_info(JZ_APP_COM_MOD,
                "NTP sync ok utc=%llu offset=%ld local=%04d-%02d-%02d "
                "%02d:%02d:%02d stratum=%u\n",
                (unsigned long long)unix_seconds, (long)timezone_offset,
                rtc_value.tm_year, rtc_value.tm_mon, rtc_value.tm_mday,
                rtc_value.tm_hour, rtc_value.tm_min, rtc_value.tm_sec,
                (unsigned int)packet.stratum);
    ret = 0;

out:
    if (sockfd >= 0) {
        close(sockfd);
    }
    mutex_unlock(&g_time_sync_lock);

    return ret;
}

static unsigned int jz_ntp_retry_delay_us(int retry_times)
{
    if (retry_times <= 1) {
        return JZ_NTP_RETRY_INITIAL_US;
    }
    if (retry_times <= 3) {
        return 15U * 1000U * 1000U;
    }
    if (retry_times <= 6) {
        return 30U * 1000U * 1000U;
    }
    return JZ_NTP_RETRY_MAX_US;
}

static int jz_should_log_retry(int retry_times)
{
    return retry_times <= 2 ||
           (retry_times > 0 && (retry_times & (retry_times - 1)) == 0);
}

static void jz_time_service_thread(void *user_data)
{
    int retry_times = 0;
#ifdef CONFIG_NET_MANAGE
    int wifi_status;
    int wifi_wait_times = 0;
#endif

    (void)user_data;

    while (1) {
        time_cfg = jz_get_time_config();
        if (!time_cfg->auto_time_enable) {
            retry_times = 0;
#ifdef CONFIG_NET_MANAGE
            wifi_wait_times = 0;
#endif
            sleep(1);
            continue;
        }
        if (retry_times > 50) {
            retry_times = 0;
            jz_log_error(JZ_APP_COM_MOD, "Time synchronization failed. Please check the network\n");
            usleep(JZ_NTP_FAIL_SYNC_PERIOD_US);
            continue;
        }
#ifdef CONFIG_NET_MANAGE
        wifi_status = wifi_status_get();
        if (wifi_status != JZ_WIFI_STATUS_AVAILABLE) {
            ++wifi_wait_times;
            if (jz_should_log_retry(wifi_wait_times)) {
                jz_log_warning(JZ_APP_COM_MOD,
                               "Wifi is not ready for NTP status=%d wait=%d\n",
                               wifi_status, wifi_wait_times);
            }
            usleep(JZ_NTP_RETRY_INITIAL_US);
            continue;
        }
        wifi_wait_times = 0;
#else
        {
            jz_log_warning(JZ_APP_COM_MOD, "Wifi is not supported\n");
            usleep(JZ_NTP_RETRY_MAX_US);
            continue;
        }
#endif
        if (jz_get_ntp_server_time()) {
            unsigned int retry_delay_us;

            ++retry_times;
            retry_delay_us = jz_ntp_retry_delay_us(retry_times);
            if (jz_should_log_retry(retry_times)) {
                jz_log_warning(JZ_APP_COM_MOD,
                               "NTP sync retry=%d next=%ums\n",
                               retry_times, retry_delay_us / 1000U);
            }
            usleep(retry_delay_us);
            continue;
        }

        jz_refresh_status_bar_time_safe();

        /* RTC keeps time locally; a successful network correction is daily. */
        retry_times = 0;
        sleep(JZ_NTP_SUCCESS_SYNC_PERIOD_S);
    }
}

void jz_time_service_init(void)
{
    mutex_init(&g_time_sync_lock);
    time_cfg = jz_get_time_config();
    (void)jz_apply_time_zone(time_cfg->time_zone);
    create_status_bar_time_task();

    jz_hmi_register_service_handler(CMD_TIME_SYNC, jz_time_sync_service_handler, "time_sync_service");
    thread_create("jz_time_service_thread", 8 * 1024, jz_time_service_thread, NULL);
}
