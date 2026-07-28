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

/* 已绑定设备使用云端下发的 device_id 作为 client_id，避免身份冲突。 */
#ifndef TIRTC_CLIENT_ID
#define TIRTC_CLIENT_ID TIRTC_DEVICE_ID
#endif

/* BOOT 键触发主动连接的目标设备 ID。 */
#ifndef TIRTC_REMOTE_DEVICE_ID
#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"
#endif

/* 本地签发 token 时需要目标设备的 secret key，仅用于联调。 */
#ifndef TIRTC_REMOTE_DEVICE_SECRET_KEY
#define TIRTC_REMOTE_DEVICE_SECRET_KEY "peer_device_secret_key"
#endif
#ifndef TIRTC_AUTO_CONNECT_AFTER_STARTED
#define TIRTC_AUTO_CONNECT_AFTER_STARTED 0
#endif

/* 本地签发 token 仅用于联调；量产时应由业务服务端签发 token。 */
#ifndef TIRTC_TOKEN_ACCESS_ID
#define TIRTC_TOKEN_ACCESS_ID "your_token_access_id"
#endif

#ifndef TIRTC_TOKEN_SECRET_KEY
#define TIRTC_TOKEN_SECRET_KEY "your_token_secret_key"
#endif

#ifndef TIRTC_TOKEN_SUBJECT
#define TIRTC_TOKEN_SUBJECT "your_subject"
#endif

#ifndef TIRTC_TOKEN_TTL_SECONDS
#define TIRTC_TOKEN_TTL_SECONDS 300
#endif
