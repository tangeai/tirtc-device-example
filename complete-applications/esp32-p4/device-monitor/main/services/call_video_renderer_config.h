#pragma once

#include "media_tuning.h"

/*
 * P4 downlink decode and presentation scheduling.
 *
 * These values describe one tightly coupled pipeline. Keep them together so a
 * scheduling experiment cannot silently change only the caller or only the
 * TinyH264 helper. They are application policy, not sdkconfig options.
 */

#define CALL_VIDEO_DECODE_MAX_WIDTH         APP_MEDIA_CALL_VIDEO_WIDTH
#define CALL_VIDEO_DECODE_MAX_HEIGHT        APP_MEDIA_CALL_VIDEO_HEIGHT
#define CALL_VIDEO_RENDER_WIDTH             APP_MEDIA_CALL_VIDEO_WIDTH
#define CALL_VIDEO_RENDER_HEIGHT            APP_MEDIA_CALL_VIDEO_HEIGHT

#define CALL_VIDEO_INPUT_SLOT_COUNT         16U
#define CALL_VIDEO_INPUT_SLOT_CAPACITY      (256U * 1024U)
#define CALL_VIDEO_SOURCE_CROP_X            0U
#define CALL_VIDEO_SOURCE_CROP_Y            0U
#define CALL_VIDEO_SOURCE_CROP_WIDTH        APP_MEDIA_CALL_VIDEO_WIDTH
#define CALL_VIDEO_SOURCE_CROP_HEIGHT       APP_MEDIA_CALL_VIDEO_HEIGHT
#define CALL_VIDEO_DECODED_SLOT_COUNT       4U
#define CALL_VIDEO_OUTPUT_SLOT_COUNT        4U

#define CALL_VIDEO_TASK_STACK_SIZE          (16U * 1024U)
#define CALL_VIDEO_MJPEG_TASK_STACK_SIZE    (8U * 1024U)
#define CALL_VIDEO_CONVERT_TASK_STACK_SIZE  (8U * 1024U)

/*
 * Full-duplex calls must leave CPU0 scheduling windows for TiRTC transport and
 * the local camera pipeline. Run TinyH264 with its bounded helper on CPU1 so a
 * complex access unit does not monopolize the decoder caller on CPU0. Keep the
 * helper above the caller and at the official component's default priority.
 * TinyH264 waits indefinitely for this helper at two slice barriers, so the
 * helper must not sit below continuously active audio playback. Capture audio
 * remains one level higher; display conversion/UI stay below the decoder.
 * The decoder owns the helper lifecycle, while this application-level switch
 * keeps the policy out of generated sdkconfig.
 */
#define CALL_VIDEO_TASK_PRIORITY             16U
#define CALL_VIDEO_H264_DUAL_TASK_ENABLE      1U
#define CALL_VIDEO_H264_HELPER_TASK_CORE      1U
#define CALL_VIDEO_H264_HELPER_TASK_PRIORITY 17U
#define CALL_VIDEO_CONVERT_TASK_PRIORITY      15U

#define CALL_VIDEO_START_TIMEOUT_MS           5000U
#define CALL_VIDEO_STOP_TIMEOUT_MS            3000U
#define CALL_VIDEO_MJPEG_DECODE_TIMEOUT_MS     100U
#define CALL_VIDEO_STATS_INTERVAL_US          (5LL * 1000LL * 1000LL)
/* The first-frame and focused stall logs are sufficient in normal firmware.
 * Per-frame boot traces are long synchronous UART writes and can themselves
 * perturb the exact startup interval being measured. Set this temporarily to
 * a small non-zero value only for a dedicated pipeline timing capture. */
#define CALL_VIDEO_STARTUP_TRACE_FRAMES        0U
#define CALL_VIDEO_TARGET_FRAME_INTERVAL_US    \
    ((1000000U + APP_MEDIA_CALL_VIDEO_FPS - 1U) / APP_MEDIA_CALL_VIDEO_FPS)
#define CALL_VIDEO_SLOW_DECODE_US              (CALL_VIDEO_TARGET_FRAME_INTERVAL_US * 2U)
#define CALL_VIDEO_INPUT_GAP_US                (CALL_VIDEO_TARGET_FRAME_INTERVAL_US * 3U)
#define CALL_VIDEO_LATENCY_RECOVERY_US          (CALL_VIDEO_TARGET_FRAME_INTERVAL_US * 3U)
#define CALL_VIDEO_DECODE_BACKLOG_YIELD_MS      2U
#define CALL_VIDEO_DECODE_HANG_TIMEOUT_US       (2LL * 1000LL * 1000LL)
#define CALL_VIDEO_STALL_LOG_INTERVAL_US       (5LL * 1000LL * 1000LL)
