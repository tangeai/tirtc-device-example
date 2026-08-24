#pragma once

/*
 * ESP32-S3 product capability boundary.
 *
 * This product publishes and plays audio only. The camera remains available
 * exclusively to QR-code workflows; it is not an RTC video source. Keeping
 * this decision in one header prevents UI, application, and protocol layers
 * from silently re-enabling a partial video path.
 */
#define APP_PRODUCT_RTC_VIDEO_ENABLED     0
#define APP_PRODUCT_IPC_AUDIO_ENABLED     1
#define APP_PRODUCT_QR_CAMERA_ENABLED     1
