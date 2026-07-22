#include "app_memory_policy.h"

#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_memory_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_psram_alloc_failures;

static void app_memory_note_psram_failure(void)
{
    taskENTER_CRITICAL(&s_memory_lock);
    s_psram_alloc_failures++;
    taskEXIT_CRITICAL(&s_memory_lock);
}

void *app_memory_alloc_psram(size_t size)
{
    void *ptr = NULL;

    if (size == 0U) {
        return NULL;
    }

    ptr = heap_caps_malloc(size, APP_MEMORY_CAPS_PSRAM);
    if (ptr == NULL) {
        app_memory_note_psram_failure();
    }
    return ptr;
}

void *app_memory_calloc_psram(size_t count, size_t size)
{
    void *ptr = NULL;

    if (count == 0U || size == 0U || count > (SIZE_MAX / size)) {
        return NULL;
    }

    ptr = heap_caps_calloc(count, size, APP_MEMORY_CAPS_PSRAM);
    if (ptr == NULL) {
        app_memory_note_psram_failure();
    }
    return ptr;
}

void app_memory_get_snapshot(app_memory_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->internal_free = heap_caps_get_free_size(APP_MEMORY_CAPS_CONTROL);
    snapshot->internal_largest = heap_caps_get_largest_free_block(APP_MEMORY_CAPS_CONTROL);
    snapshot->internal_min_free = heap_caps_get_minimum_free_size(APP_MEMORY_CAPS_CONTROL);
    snapshot->dma_free = heap_caps_get_free_size(APP_MEMORY_CAPS_DMA);
    snapshot->dma_largest = heap_caps_get_largest_free_block(APP_MEMORY_CAPS_DMA);
    snapshot->dma_min_free = heap_caps_get_minimum_free_size(APP_MEMORY_CAPS_DMA);
    snapshot->psram_free = heap_caps_get_free_size(APP_MEMORY_CAPS_PSRAM);
    snapshot->psram_largest = heap_caps_get_largest_free_block(APP_MEMORY_CAPS_PSRAM);
    snapshot->psram_min_free = heap_caps_get_minimum_free_size(APP_MEMORY_CAPS_PSRAM);

    taskENTER_CRITICAL(&s_memory_lock);
    snapshot->psram_alloc_failures = s_psram_alloc_failures;
    taskEXIT_CRITICAL(&s_memory_lock);
}
