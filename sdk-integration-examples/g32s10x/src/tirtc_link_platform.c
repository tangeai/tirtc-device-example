#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <driver/network/wireless.h>
#include <driver/dtrng.h>
#include <driver/systick.h>
#include <lwip/api.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <spinlock.h>

#include "tirtc_link_config.h"
#include "tirtc_link_defaults.h"
#include "tirtc_link_platform.h"

#define LINK_NTP_PORT 123U
#define LINK_NTP_PACKET_SIZE 48U
#define LINK_NTP_UNIX_DELTA 2208988800UL
#define LINK_VALID_UNIX_TIME 1704067200UL
#define LINK_CLIENT_ID_BYTES 13U

static DEFINE_SPINLOCK(g_clock_lock);
static bool g_platform_initialized;
static bool g_clock_ready;
static uint32_t g_clock_epoch_sec;
static uint32_t g_clock_epoch_usec;
static uint64_t g_clock_base_us;
static uint64_t g_next_wifi_join_ms;
static char g_wifi_ssid[] = TIRTC_LINK_WIFI_SSID;
static char g_wifi_password[] = TIRTC_LINK_WIFI_PASSWORD;

static void link_wifi_event(wireless_event event, void *data)
{
    (void)event;
    (void)data;
}

static uint32_t link_read_u32_be(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | (uint32_t)value[3];
}

static void link_set_wall_time(uint32_t seconds, uint32_t usec)
{
    unsigned long flags;

    spin_lock_irqsave(&g_clock_lock, flags);
    g_clock_epoch_sec = seconds;
    g_clock_epoch_usec = usec;
    g_clock_base_us = systick_get_time_us();
    g_clock_ready = true;
    spin_unlock_irqrestore(&g_clock_lock, flags);
}

int gettimeofday(struct timeval *tv, void *tz_data)
{
    struct timezone *tz = tz_data;
    uint64_t base_us;
    uint64_t now_us;
    uint64_t elapsed_us;
    uint64_t wall_us;
    uint32_t epoch_sec;
    uint32_t epoch_usec;
    bool ready;
    unsigned long flags;

    spin_lock_irqsave(&g_clock_lock, flags);
    ready = g_clock_ready;
    base_us = g_clock_base_us;
    epoch_sec = g_clock_epoch_sec;
    epoch_usec = g_clock_epoch_usec;
    spin_unlock_irqrestore(&g_clock_lock, flags);

    now_us = systick_get_time_us();
    if (tv != NULL) {
        if (ready) {
            elapsed_us = now_us - base_us;
            wall_us = (uint64_t)epoch_sec * 1000000ULL + epoch_usec +
                      elapsed_us;
            tv->tv_sec = (time_t)(wall_us / 1000000ULL);
            tv->tv_usec = (suseconds_t)(wall_us % 1000000ULL);
        } else {
            tv->tv_sec = (time_t)(now_us / 1000000ULL);
            tv->tv_usec = (suseconds_t)(now_us % 1000000ULL);
        }
    }
    if (tz != NULL) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

long HAL_Random(void)
{
    return (long)dtrng_read_random_data();
}

#if !LWIP_IPV6
#ifdef netconn_gethostbyname_addrtype
#undef netconn_gethostbyname_addrtype
#endif
err_t netconn_gethostbyname_addrtype(const char *name, ip_addr_t *address,
                                     u8_t address_type)
{
    (void)address_type;
    return netconn_gethostbyname(name, address);
}
#endif

int tirtc_link_platform_init(void)
{
    if (g_platform_initialized) {
        return 0;
    }
    register_service_callback(link_wifi_event);
    wifi_ops_init();
    g_next_wifi_join_ms = systick_get_time_ms() +
                          TIRTC_LINK_WIFI_JOIN_DELAY_MS;
    g_platform_initialized = true;
    printf("[TEST][INFO] 启动配置 | WiFi=%s channel=%d\n",
           tirtc_link_platform_wifi_configured() ? "已配置" : "未配置",
           TIRTC_LINK_WIFI_CHANNEL);
    return 0;
}

void tirtc_link_platform_poll(void)
{
    uint64_t now_ms;

    if (!g_platform_initialized || !tirtc_link_platform_wifi_configured() ||
        tirtc_link_platform_network_ready()) {
        return;
    }
    now_ms = systick_get_time_ms();
    if (now_ms < g_next_wifi_join_ms) {
        return;
    }
    wifi_join(g_wifi_ssid, g_wifi_password, TIRTC_LINK_WIFI_CHANNEL);
    g_next_wifi_join_ms = now_ms + TIRTC_LINK_WIFI_RETRY_MS;
}

bool tirtc_link_platform_wifi_configured(void)
{
    return g_wifi_ssid[0] != '\0';
}

bool tirtc_link_platform_network_ready(void)
{
    return wifi_status_get() == JZ_WIFI_STATUS_AVAILABLE;
}

bool tirtc_link_platform_time_ready(void)
{
    bool ready;
    unsigned long flags;

    spin_lock_irqsave(&g_clock_lock, flags);
    ready = g_clock_ready;
    spin_unlock_irqrestore(&g_clock_lock, flags);
    return ready;
}

int tirtc_link_platform_sync_time(void)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct sockaddr_in server;
    struct timeval timeout;
    uint8_t packet[LINK_NTP_PACKET_SIZE];
    uint32_t ntp_seconds;
    uint32_t fraction;
    uint32_t unix_seconds;
    uint32_t usec;
    int socket_fd = -1;
    int result = TIRTC_LINK_E_NTP_NETWORK;
    int received;

    if (!tirtc_link_platform_network_ready()) {
        return -1;
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(TIRTC_LINK_NTP_SERVER, NULL, &hints, &addresses) != 0 ||
        addresses == NULL || addresses->ai_addr == NULL ||
        addresses->ai_addrlen < sizeof(server)) {
        result = TIRTC_LINK_E_NTP_DNS;
        goto out;
    }

    memset(&server, 0, sizeof(server));
    memcpy(&server, addresses->ai_addr, sizeof(server));
    server.sin_port = htons(LINK_NTP_PORT);
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        result = TIRTC_LINK_E_NTP_SOCKET;
        goto out;
    }
    timeout.tv_sec = (time_t)(TIRTC_LINK_NTP_TIMEOUT_MS / 1000U);
    timeout.tv_usec =
        (suseconds_t)((TIRTC_LINK_NTP_TIMEOUT_MS % 1000U) * 1000U);
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        result = TIRTC_LINK_E_NTP_SOCKET_OPTION;
        goto out;
    }

    memset(packet, 0, sizeof(packet));
    packet[0] = 0x23;
    if (sendto(socket_fd, packet, sizeof(packet), 0,
               (struct sockaddr *)&server, sizeof(server)) !=
        (int)sizeof(packet)) {
        result = TIRTC_LINK_E_NTP_SEND;
        goto out;
    }
    received = recvfrom(socket_fd, packet, sizeof(packet), 0, NULL, NULL);
    if (received != (int)sizeof(packet)) {
        result = TIRTC_LINK_E_NTP_RECEIVE;
        goto out;
    }
    if ((packet[0] >> 6) == 3U || (packet[0] & 0x07U) != 4U ||
        packet[1] == 0U || packet[1] > 15U) {
        result = TIRTC_LINK_E_NTP_RESPONSE;
        goto out;
    }
    ntp_seconds = link_read_u32_be(&packet[40]);
    fraction = link_read_u32_be(&packet[44]);
    if (ntp_seconds <= LINK_NTP_UNIX_DELTA) {
        result = TIRTC_LINK_E_NTP_TIMESTAMP;
        goto out;
    }
    unix_seconds = ntp_seconds - LINK_NTP_UNIX_DELTA;
    if (unix_seconds < LINK_VALID_UNIX_TIME) {
        result = TIRTC_LINK_E_NTP_TIMESTAMP;
        goto out;
    }
    usec = (uint32_t)(((uint64_t)fraction * 1000000ULL) >> 32);
    link_set_wall_time(unix_seconds, usec);
    printf("[TEST][PASS] 时间校准 | unix=%lu stratum=%u\n",
           (unsigned long)unix_seconds, packet[1]);
    result = 0;

out:
    if (socket_fd >= 0) {
        close(socket_fd);
    }
    if (addresses != NULL) {
        freeaddrinfo(addresses);
    }
    memset(packet, 0, sizeof(packet));
    return result;
}

int tirtc_link_platform_get_client_id(char *client_id, size_t capacity)
{
    struct netif *netif;
    uint8_t mac[6] = {0};
    bool found = false;

    if (client_id == NULL || capacity < LINK_CLIENT_ID_BYTES) {
        return -1;
    }
    LOCK_TCPIP_CORE();
    NETIF_FOREACH(netif) {
        if (netif->type == NETIF_TYPE_WIFI &&
            netif->hwaddr_len >= sizeof(mac)) {
            memcpy(mac, netif->hwaddr, sizeof(mac));
            found = true;
            break;
        }
    }
    UNLOCK_TCPIP_CORE();
    if (!found) {
        return -1;
    }
    snprintf(client_id, capacity, "%02X%02X%02X%02X%02X%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
