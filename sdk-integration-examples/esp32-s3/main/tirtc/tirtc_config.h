#pragma once

/* TiRTC 业务配置：接入时主要改这里。 */

#if __has_include("local_config.h")
#include "local_config.h"
#endif

/* 留空时使用 SDK 默认服务入口；正式 2.2.1 S3 包仅支持 HTTP。 */
#ifndef TIRTC_SERVICE_ENDPOINT
#define TIRTC_SERVICE_ENDPOINT ""
#endif

#ifndef TIRTC_DEVICE_ID
#define TIRTC_DEVICE_ID "your_device_id"
#endif

#ifndef TIRTC_DEVICE_SECRET_KEY
#define TIRTC_DEVICE_SECRET_KEY "your_device_secret_key"
#endif

/* 1..64 个可打印 ASCII 字符；同一 device_id 后续必须复用首次绑定值。 */
#ifndef TIRTC_CLIENT_ID
#define TIRTC_CLIENT_ID "your_stable_printable_client_id"
#endif

/* BOOT 键触发主动连接的目标设备 ID。 */
#ifndef TIRTC_REMOTE_DEVICE_ID
#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"
#endif

/*
 * 短时一次性连接 token 必须由业务服务端或 TiRTC DevTools 签发。
 * 应用 AccessKeyId/SecretKeyId 不得写入设备固件。
 */
#ifndef TIRTC_CONNECT_TOKEN
#define TIRTC_CONNECT_TOKEN ""
#endif

#ifndef TIRTC_AUTO_CONNECT_AFTER_STARTED
#define TIRTC_AUTO_CONNECT_AFTER_STARTED 0
#endif

#ifndef TIRTC_CONNECT_CACHE_ENABLED
#define TIRTC_CONNECT_CACHE_ENABLED 1
#endif

#ifndef TIRTC_LOG_LEVEL
#define TIRTC_LOG_LEVEL 5
#endif
