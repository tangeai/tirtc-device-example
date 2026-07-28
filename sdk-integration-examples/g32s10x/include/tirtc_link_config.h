#ifndef TIRTC_LINK_CONFIG_H
#define TIRTC_LINK_CONFIG_H

/*
 * Customer configuration template.
 *
 * The repository copy contains no credentials. During local builds the root
 * config/local_config.h replaces this file inside the clean Ingenic SDK.
 * Never commit passwords, device secrets or one-time tokens.
 */

/* Required network settings. Channel 0 lets the ATBM driver scan. */
#define TIRTC_LINK_WIFI_SSID ""
#define TIRTC_LINK_WIFI_PASSWORD ""
#define TIRTC_LINK_WIFI_CHANNEL 0

/* Required TiRTC device identity and service endpoint. */
#define TIRTC_LINK_NTP_SERVER "ntp.aliyun.com"
#define TIRTC_LINK_SERVICE_ENDPOINT "http://ep-tirtc.tange365.com"
#define TIRTC_LINK_DEVICE_ID ""
#define TIRTC_LINK_DEVICE_SECRET ""

/* Optional outbound connection. Keep disabled for an inbound-only device. */
#define TIRTC_LINK_AUTO_CONNECT 0
#define TIRTC_LINK_REMOTE_DEVICE_ID ""
#define TIRTC_LINK_REMOTE_TOKEN ""

/* Prerecorded output. Stream IDs must be different values from 0 to 15. */
#define TIRTC_LINK_ENABLE_SAMPLE_MEDIA 1
#define TIRTC_LINK_SAMPLE_AUTO_PUBLISH 1
#define TIRTC_LINK_SAMPLE_VIDEO_STREAM_ID 11U
#define TIRTC_LINK_SAMPLE_AUDIO_STREAM_ID 10U
#define TIRTC_LINK_SAMPLE_START_DELAY_MS 700U
#define TIRTC_LINK_SAMPLE_AUDIO_PACKET_MS 20U

/* Diagnostics. Level 2 keeps SDK error/warning logs. Zero disables heartbeat. */
#define TIRTC_LINK_SDK_LOG_LEVEL 2
#define TIRTC_LINK_STATUS_LOG_INTERVAL_MS 30000U

/* Memory and backpressure. Raise only after checking the device heap budget. */
#define TIRTC_LINK_MAX_SEND_BUFFER (128U * 1024U)
#define TIRTC_LINK_SAMPLE_VIDEO_BUFFER_PERCENT 80U
#define TIRTC_LINK_SAMPLE_AUDIO_BUFFER_PERCENT 95U

/* Retry and task tuning. Defaults suit the delivered G32S10X image. */
#define TIRTC_LINK_WIFI_JOIN_DELAY_MS 2000U
#define TIRTC_LINK_WIFI_RETRY_MS 10000U
#define TIRTC_LINK_NTP_TIMEOUT_MS 5000U
#define TIRTC_LINK_NTP_RETRY_MS 30000U
#define TIRTC_LINK_START_RETRY_MS 10000U
#define TIRTC_LINK_WORKER_STACK_BYTES 12288U
#define TIRTC_LINK_WORKER_POLL_MS 250U
#define TIRTC_LINK_SAMPLE_WORKER_STACK_BYTES 6144U
#define TIRTC_LINK_SAMPLE_WORKER_POLL_MS 10U

#endif
