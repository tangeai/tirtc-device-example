#pragma once

/* TiRTC 业务配置：接入时主要改这里。 */

#ifndef TIRTC_SERVICE_ENDPOINT
#define TIRTC_SERVICE_ENDPOINT "http://ep-tirtc.tange365.com"
#endif

#ifndef TIRTC_DEVICE_ID
#define TIRTC_DEVICE_ID "your_device_id"
#endif

#ifndef TIRTC_DEVICE_SECRET_KEY
#define TIRTC_DEVICE_SECRET_KEY "your_device_secret"
#endif

/*
 * 设备端启动必填的稳定硬件/生产标识。
 * 同一个 device_id 首次上线后会绑定当次 client_id，后续应保持不变。
 */
#ifndef TIRTC_CLIENT_ID
#define TIRTC_CLIENT_ID "your_client_id"
#endif

/* AppId 对设备端可选；如业务侧要求设备端上报，可填真实 AppId。 */
#ifndef TIRTC_APP_ID
#define TIRTC_APP_ID ""
#endif

/* BOOT 键触发主动连接的目标设备 ID。 */
#ifndef TIRTC_REMOTE_DEVICE_ID
#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"
#endif

#ifndef TIRTC_AUTO_CONNECT_AFTER_STARTED
#define TIRTC_AUTO_CONNECT_AFTER_STARTED 0
#endif

/*
 * 连接建立后是否立即启动本机测试媒体推流。
 * 0: 等待对端订阅后再推送本机 H264/PCMA。
 * 1: 主动连接或远端呼入建立后立即推送本机 H264/PCMA。
 */
#ifndef TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT
#define TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT 1
#endif

/* 本地签发 token 仅用于快速验证；量产时应由业务服务端签发 token。 */
#ifndef TIRTC_TOKEN_ACCESS_ID
#define TIRTC_TOKEN_ACCESS_ID "your_token_access_id"
#endif

#ifndef TIRTC_TOKEN_SECRET_KEY
#define TIRTC_TOKEN_SECRET_KEY "your_token_secret_key"
#endif

#ifndef TIRTC_TOKEN_SUBJECT
#define TIRTC_TOKEN_SUBJECT "devtools-cli"
#endif

#ifndef TIRTC_TOKEN_TTL_SECONDS
#define TIRTC_TOKEN_TTL_SECONDS 300
#endif
