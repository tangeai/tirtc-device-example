#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "os/thread.h"
#include "third_party/lvgl/jz_lvgl_common/jz_lvgl_mutex.h"
#include "third_party/lvgl/lvgl/lvgl.h"
#include "tirtc_screen_debug.h"

#define SCREEN_DEBUG_PORT 8080
#define SCREEN_DEBUG_STACK_SIZE (24 * 1024)
#define SCREEN_DEBUG_REQUEST_SIZE 512

static bool g_started;

static bool get_ipv4(char *buffer, size_t buffer_size)
{
    struct netif *netif;

    NETIF_FOREACH(netif) {
        const ip4_addr_t *address = netif_ip4_addr(netif);
        if (netif_is_up(netif) && !ip4_addr_isany_val(*address) &&
            !ip4_addr_isloopback(address) &&
            ip4addr_ntoa_r(address, buffer, (int)buffer_size) != NULL) {
            return true;
        }
    }
    if (buffer_size != 0U) {
        buffer[0] = '\0';
    }
    return false;
}

static const char g_index_html[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>G32 Screen</title><style>"
    "body{margin:0;background:#101820;color:#eef5f8;font:14px sans-serif;text-align:center}"
    "header{padding:12px}img{max-width:calc(100vw - 20px);max-height:calc(100vh - 70px);"
    "image-rendering:auto;border:1px solid #49616f;background:#000}"
    "</style><header>G32S10X Screen <span id=s>connecting</span></header>"
    "<img id=i src='/screen.bmp'><script>"
    "const i=document.getElementById('i'),s=document.getElementById('s');"
    "function r(){i.onload=()=>{s.textContent='online';setTimeout(r,500)};"
    "i.onerror=()=>{s.textContent='retrying';setTimeout(r,1500)};"
    "i.src='/screen.bmp?t='+Date.now()}r()</script>";

static void write_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static int send_all(int fd, const void *data, size_t size)
{
    const uint8_t *cursor = data;
    while (size != 0U) {
        int sent = send(fd, cursor, size, 0);
        if (sent <= 0) {
            return -1;
        }
        cursor += sent;
        size -= (size_t)sent;
    }
    return 0;
}

static int send_response(int fd, const char *type, const void *body, size_t size)
{
    char header[192];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                          "Content-Length: %lu\r\nCache-Control: no-store\r\n"
                          "Connection: close\r\n\r\n",
                          type, (unsigned long)size);
    if (length <= 0 || length >= (int)sizeof(header)) {
        return -1;
    }
    return send_all(fd, header, (size_t)length) || send_all(fd, body, size);
}

static int send_error(int fd, int status, const char *reason)
{
    char body[96];
    char header[192];
    int body_len = snprintf(body, sizeof(body), "%d %s\n", status, reason);
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                              "Content-Length: %d\r\nConnection: close\r\n\r\n",
                              status, reason, body_len);
    if (body_len <= 0 || header_len <= 0) {
        return -1;
    }
    return send_all(fd, header, (size_t)header_len) ||
           send_all(fd, body, (size_t)body_len);
}

static int capture_bmp(uint8_t **bmp, size_t *bmp_size)
{
    lv_img_dsc_t snapshot = {0};
    lv_obj_t *screen;
    uint8_t *snapshot_data;
    uint8_t *output;
    uint32_t snapshot_size;
    uint32_t width;
    uint32_t height;
    uint32_t row_stride;
    size_t output_size;

    *bmp = NULL;
    *bmp_size = 0;
    jz_lv_mutex_lock();
    screen = lv_scr_act();
    snapshot_size = lv_snapshot_buf_size_needed(screen, LV_IMG_CF_TRUE_COLOR);
    snapshot_data = snapshot_size ? malloc(snapshot_size) : NULL;
    if (snapshot_data == NULL ||
        lv_snapshot_take_to_buf(screen, LV_IMG_CF_TRUE_COLOR, &snapshot,
                                snapshot_data, snapshot_size) != LV_RES_OK) {
        jz_lv_mutex_unlock();
        free(snapshot_data);
        return -1;
    }
    width = snapshot.header.w;
    height = snapshot.header.h;
    row_stride = (width * 3U + 3U) & ~3U;
    output_size = 54U + (size_t)row_stride * height;
    jz_lv_mutex_unlock();

    output = calloc(1, output_size);
    if (output == NULL) {
        free(snapshot_data);
        return -1;
    }
    output[0] = 'B';
    output[1] = 'M';
    write_u32(output + 2, (uint32_t)output_size);
    write_u32(output + 10, 54);
    write_u32(output + 14, 40);
    write_u32(output + 18, width);
    write_u32(output + 22, height);
    write_u16(output + 26, 1);
    write_u16(output + 28, 24);
    write_u32(output + 34, row_stride * height);

    for (uint32_t y = 0; y < height; ++y) {
        const lv_color_t *src = (const lv_color_t *)(snapshot.data +
                                (size_t)y * width * sizeof(lv_color_t));
        uint8_t *dst = output + 54U + (size_t)(height - 1U - y) * row_stride;
        for (uint32_t x = 0; x < width; ++x) {
            lv_color32_t color = {.full = lv_color_to32(src[x])};
            dst[x * 3U] = color.ch.blue;
            dst[x * 3U + 1U] = color.ch.green;
            dst[x * 3U + 2U] = color.ch.red;
        }
    }
    free(snapshot_data);
    *bmp = output;
    *bmp_size = output_size;
    return 0;
}

static void handle_client(int fd)
{
    char request[SCREEN_DEBUG_REQUEST_SIZE] = {0};
    uint8_t *bmp;
    size_t bmp_size;
    int received = recv(fd, request, sizeof(request) - 1U, 0);

    if (received <= 0) {
        return;
    }
    if (strncmp(request, "GET /screen.bmp", 15) == 0) {
        if (capture_bmp(&bmp, &bmp_size) == 0) {
            (void)send_response(fd, "image/bmp", bmp, bmp_size);
            free(bmp);
        } else {
            (void)send_error(fd, 503, "Capture Failed");
        }
    } else if (strncmp(request, "GET /api/status", 15) == 0) {
        char ip[IP4ADDR_STRLEN_MAX] = {0};
        char status[160];
        int length;

        (void)get_ipv4(ip, sizeof(ip));
        length = snprintf(status, sizeof(status),
                          "{\"ok\":true,\"chip\":\"G32S10X\",\"ip\":\"%s\","
                          "\"width\":480,\"height\":854,\"port\":8080}",
                          ip);
        if (length > 0 && length < (int)sizeof(status)) {
            (void)send_response(fd, "application/json", status, (size_t)length);
        } else {
            (void)send_error(fd, 500, "Status Failed");
        }
    } else if (strncmp(request, "GET / ", 6) == 0) {
        (void)send_response(fd, "text/html; charset=utf-8",
                            g_index_html, sizeof(g_index_html) - 1U);
    } else {
        (void)send_error(fd, 404, "Not Found");
    }
}

static void screen_debug_task(void *arg)
{
    struct sockaddr_in address = {0};
    char ip[IP4ADDR_STRLEN_MAX] = {0};
    int listen_fd;
    int reuse = 1;
    (void)arg;

    address.sin_family = AF_INET;
    address.sin_port = htons(SCREEN_DEBUG_PORT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    for (;;) {
        listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd >= 0) {
            (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                             &reuse, sizeof(reuse));
            if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) == 0 &&
                listen(listen_fd, 2) == 0) {
                break;
            }
            close(listen_fd);
        }
        printf("[screen_debug] network stack not ready errno=%d retry=1000ms\n",
               errno);
        sleep(1);
    }

    printf("[screen_debug] listening port=%d waiting_for_ipv4=1\n",
           SCREEN_DEBUG_PORT);
    while (!get_ipv4(ip, sizeof(ip))) {
        sleep(1);
    }
    printf("[screen_debug] ready ip=%s url=http://%s:%d/\n",
           ip, ip, SCREEN_DEBUG_PORT);
    for (;;) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd >= 0) {
            handle_client(client_fd);
            close(client_fd);
        }
    }
}

int tirtc_screen_debug_start(void)
{
    if (g_started) {
        return 0;
    }
    g_started = true;
    if (thread_create("tirtc_screen", SCREEN_DEBUG_STACK_SIZE,
                      screen_debug_task, NULL) == NULL) {
        g_started = false;
        return -1;
    }
    return 0;
}
