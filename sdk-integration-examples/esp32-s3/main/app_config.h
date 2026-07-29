#pragma once

/* 系统侧配置。TiRTC 业务参数放在 tirtc/tirtc_config.h。 */

#if __has_include("local_config.h")
#include "local_config.h"
#endif

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID "your_wifi_ssid"
#endif

#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD "your_wifi_password"
#endif

#ifndef APP_BOOT_BUTTON_GPIO
#define APP_BOOT_BUTTON_GPIO 0
#endif

#ifndef APP_BOOT_BUTTON_ACTIVE_LEVEL
#define APP_BOOT_BUTTON_ACTIVE_LEVEL 0
#endif
