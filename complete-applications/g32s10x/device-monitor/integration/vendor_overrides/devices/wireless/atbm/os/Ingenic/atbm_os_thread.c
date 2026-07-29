#include "atbm_hal.h"
#include "atbm_os_thread.h"
#include <string.h>

#define ATBM_WIFI_STACK_SIZE       (8U * 1024U)
#ifndef ATBM_WIFI_RX_STACK_SIZE
#define ATBM_WIFI_RX_STACK_SIZE    (24U * 1024U)
#endif

static unsigned int atbm_thread_stack_size(const char *name)
{
    if (name != NULL && strcmp(name, "atbm_rx") == 0)
        return ATBM_WIFI_RX_STACK_SIZE;

    return ATBM_WIFI_STACK_SIZE;
}

pAtbm_thread_t atbm_createThread(char *name,thread_func_t task,atbm_void *p_arg,int prio)
{
	thread_ptr_t th;
    unsigned int stack_size = atbm_thread_stack_size(name);

    if (stack_size != ATBM_WIFI_STACK_SIZE)
        iot_printf("[atbm_wifi] thread %s stack=%u bytes\n", name, stack_size);

    th=thread_create(name, stack_size, task, p_arg);
	if(!th){
		iot_printf("\"%s\" thread create fail\n",name);
		return NULL;
	}
	thread_set_priority(th, prio);
	return th;
}

int atbm_stopThread(pAtbm_thread_t thread_id)
{
	thread_delete(thread_id);
	return 0;
}

int atbm_ThreadStopEvent(pAtbm_thread_t thread_id)
{
	return 0;
}

void* atbm_getCurThreadId(void)
{
	return (void*)thread_get_current();
}
