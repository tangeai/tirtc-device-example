#ifndef DEVICE_UTF8_H
#define DEVICE_UTF8_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool device_utf8_validate(const char *text);

size_t device_utf8_sanitize_line(const char *source,
                                 char *destination,
                                 size_t destination_size,
                                 size_t byte_limit);

#ifdef __cplusplus
}
#endif

#endif
