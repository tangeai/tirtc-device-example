#pragma once

/*
 * Copy only the settings needed for local testing to local_config.h.
 * local_config.h is ignored by Git and must never be included in a release.
 */

#define APP_WIFI_SSID "your_wifi_ssid"
#define APP_WIFI_PASSWORD "your_wifi_password"

#define TIRTC_DEVICE_ID "your_device_id"
#define TIRTC_DEVICE_SECRET_KEY "your_device_secret_key"
#define TIRTC_CLIENT_ID "your_stable_printable_client_id"

#define TIRTC_REMOTE_DEVICE_ID "peer_device_id"

/*
 * Issue a short-lived, one-time token from your business backend or TiRTC
 * DevTools. Application signing secrets must not be stored in device firmware.
 */
#define TIRTC_CONNECT_TOKEN "short_lived_one_time_connect_token"

/* Leave empty to use the SDK default endpoint. This 2.2.1 package is HTTP-only. */
#define TIRTC_SERVICE_ENDPOINT ""

#define TIRTC_AUTO_CONNECT_AFTER_STARTED 0
