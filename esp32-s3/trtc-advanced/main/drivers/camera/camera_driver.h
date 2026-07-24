#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
	CAMERA_DRIVER_PIXEL_FORMAT_GRAYSCALE = 0,
	CAMERA_DRIVER_PIXEL_FORMAT_RGB565,
} camera_driver_pixel_format_t;

typedef struct {
	const uint8_t *data;
	size_t data_len;
	uint16_t width;
	uint16_t height;
	camera_driver_pixel_format_t pixel_format;
	void *owner;
} camera_driver_frame_t;

bool camera_driver_is_configured(void);
bool camera_driver_is_initialized(void);
esp_err_t camera_driver_init(void);
esp_err_t camera_driver_capture(camera_driver_frame_t *frame);
void camera_driver_release(camera_driver_frame_t *frame);
esp_err_t camera_driver_deinit(void);
