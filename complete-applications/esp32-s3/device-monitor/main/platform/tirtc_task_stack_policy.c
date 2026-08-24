#include <string.h>

#include "esp_log.h"

/*
 * TiRTC 2.3.0 creates the TGTRP connection worker as "rtc_thread" with an
 * 8 KiB external-RAM stack. Device-to-device call setup has been observed to
 * overflow that stack immediately after media starts. Keep this compatibility
 * policy at the SDK boundary rather than spreading stack workarounds through
 * the application state machines.
 */
#define TIRTC_RTC_THREAD_MIN_STACK_BYTES (16 * 1024)

typedef void (*tirtc_thread_routine_t)(void *arg);

extern int __real_freertos_ThreadCreateWithStackSize(void *thread_handle,
                                                      tirtc_thread_routine_t routine,
                                                      void *arg,
                                                      int stack_size,
                                                      const char *name);

int __wrap_freertos_ThreadCreateWithStackSize(void *thread_handle,
                                              tirtc_thread_routine_t routine,
                                              void *arg,
                                              int stack_size,
                                              const char *name)
{
    int effective_stack_size = stack_size;

    if (name != NULL && strcmp(name, "rtc_thread") == 0 &&
        effective_stack_size < TIRTC_RTC_THREAD_MIN_STACK_BYTES) {
        effective_stack_size = TIRTC_RTC_THREAD_MIN_STACK_BYTES;
        ESP_LOGI("tirtc_task_stack",
                 "TiRTC task stack adjusted: name=%s requested=%d effective=%d",
                 name,
                 stack_size,
                 effective_stack_size);
    }

    return __real_freertos_ThreadCreateWithStackSize(thread_handle,
                                                      routine,
                                                      arg,
                                                      effective_stack_size,
                                                      name);
}
