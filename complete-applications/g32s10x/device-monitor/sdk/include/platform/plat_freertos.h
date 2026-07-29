#ifndef __plat_freertos_h__
#define __plat_freertos_h__

/* V9.0.0s and above */

#include "basedef.h"

#define SA_FILE void

#include "FreeRTOS.h"
#ifdef ESP_PLATFORM
#include "freertos/task.h"
#include "freertos/semphr.h"
#elif defined(__EC71X__) || defined(__G32S10X__)
#include "task.h"
#include "semphr.h"
#else
#include "FreeRTOS/task.h"
#include "FreeRTOS/semphr.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define SA_srand(x) srandom(x)
#define SA_rand() random()


#define SA_INFINITE 0xffffffff

#define __need_SA_platInit__
void SA_platInit();
/*
 * Event
 */
#ifdef __G32S10X__
typedef SemaphoreHandle_t SA_EVENT;

#define SA_EventInit(x) (x = xSemaphoreCreateBinary())
#define SA_EventUninit(x) vSemaphoreDelete(x)
#define SA_EventSet(x) xSemaphoreGive(x)
#define SA_EventWait(x) xSemaphoreTake(x, portMAX_DELAY)
static SA_BOOL _FreeRtos_EventWaitTimed(SA_EVENT e, uint32_t wait_ms)
{
    return xSemaphoreTake(e, wait_ms==SA_INFINITE?portMAX_DELAY:pdMS_TO_TICKS(wait_ms)) == pdPASS;
}
#define SA_EventWaitTimed(x, wait_ms) _FreeRtos_EventWaitTimed(x, wait_ms)
#else
typedef StaticSemaphore_t SA_EVENT;

#define SA_EventInit(x) xSemaphoreCreateBinaryStatic(&x)
#define SA_EventUninit(x)
#define SA_EventSet(x) xSemaphoreGive((SemaphoreHandle_t)&x)
#define SA_EventWait(x) xSemaphoreTake((SemaphoreHandle_t)&x, portMAX_DELAY)
static SA_BOOL _FreeRtos_EventWaitTimed(SA_EVENT *e, uint32_t wait_ms)
{
    return xSemaphoreTake((SemaphoreHandle_t)e, wait_ms==SA_INFINITE?portMAX_DELAY:pdMS_TO_TICKS(wait_ms)) == pdPASS;
}
#define SA_EventWaitTimed(x, wait_ms) _FreeRtos_EventWaitTimed(&x, wait_ms)
#endif

/*
 * Mutex
 */
#ifdef __G32S10X__
typedef SemaphoreHandle_t SA_MUTEX;

#define SA_MutexInit(x) (x = xSemaphoreCreateMutex())
#define SA_MutexUninit(x) vSemaphoreDelete(x)
#define SA_MutexLock(x) xSemaphoreTake(x, portMAX_DELAY)
#define SA_MutexTryLock(x) xSemaphoreTake(x, 0)
#define SA_MutexUnlock(x) xSemaphoreGive(x)
#else
typedef StaticSemaphore_t SA_MUTEX;

#define SA_MutexInit(x) xSemaphoreCreateMutexStatic(&x)
#define SA_MutexUninit(x)
#define SA_MutexLock(x) xSemaphoreTake((SemaphoreHandle_t)&x, portMAX_DELAY)
#define SA_MutexTryLock(x) xSemaphoreTake((SemaphoreHandle_t)&x, 0)
#define SA_MutexUnlock(x) xSemaphoreGive((SemaphoreHandle_t)&x)
#endif

/*
 * Semaphore
 */
#ifdef __G32S10X__
typedef SemaphoreHandle_t SA_SEM;

#define SA_SemInit(x, max_val, init_val) (x = xSemaphoreCreateCounting(max_val, init_val))
#define SA_SemUninit(x) vSemaphoreDelete(x)
#define SA_SemWait(x) xSemaphoreTake(x, portMAX_DELAY)
#define SA_SemPost(x) xSemaphoreGive(x)
#else
typedef StaticSemaphore_t SA_SEM;

#define SA_SemInit(x, max_val, init_val) xSemaphoreCreateCountingStatic(max_val, init_val, &x)
#define SA_SemUninit(x)
#define SA_SemWait(x) xSemaphoreTake((SemaphoreHandle_t)&x, portMAX_DELAY)
#define SA_SemPost(x) xSemaphoreGive((SemaphoreHandle_t)&x)
#endif

#include "plat_freertos_thread.h"

/*
 * Time
 */
#define SA_GetTickCount() xTaskGetTickCount()
#define SA_Sleep(ms) vTaskDelay(pdMS_TO_TICKS(ms))

//#define SA_Tick2Ms(tick) (configTICK_RATE_HZ==1000 ? (tick) : (configTICk_RATE_HZ<1000) ? ((tick)*(1000/configTICK_RATE_HZ) : ((tick)*1000U/configTICK_RATE_HZ)))
#if 1
#define SA_Tick2Ms(tick) ((uint64_t)(tick)*1000/configTICK_RATE_HZ)
#else
#if configTICK_RATE_HZ == 1000
#define SA_Tick2Ms(tick) (tick)
#elif configTICK_RATE_HZ == 100
#define SA_Tick2Ms(tick) (uint64_t)(tick * 10)
#else
#define SA_Tick2Ms(tick) pdTICKS_TO_MS(tick)
#endif
#endif

#define SA_time(t) time(t)


//#define SA_srand(x) srand(x)
//#define SA_rand() rand()

#ifdef __cplusplus
}
#endif

#endif
