#ifndef __plat_freertos_thread_h__
#define __plat_freertos_thread_h__

#ifdef __cplusplus
extern "C" {
#endif

#define SA_THREAD_RETTYPE void
#define SA_THREAD_RETVALUE(r)
typedef SA_THREAD_RETTYPE (*SA_ThreadRoutine)(void*);

#define DECL_THREAD(func, param) void func(void *param)
#define RET_THREAD(v) return


struct freeRTOSThreadWrapper;
typedef struct freeRTOSThreadWrapper *SA_HTHREAD;

#define SA_HTHREAD_IS_VALID(h) (h != NULL)
#define SA_HTHREAD_CLEAR(h) h = NULL

void SA_ThreadWaitUntilTerminate(SA_HTHREAD h);

SA_HTHREAD SA_ThreadGetCurrentHandle();

void SA_ThreadCloseHandle(SA_HTHREAD hThread);

SA_BOOL freertos_ThreadCreateWithStackSize(SA_HTHREAD *h, SA_ThreadRoutine routine, void *arg, int stack_size, const char *name);
#define SA_ThreadCreateWithStackSize(hthd, routine, arg, stack_size, name) freertos_ThreadCreateWithStackSize(&hthd, routine, arg, stack_size, name)
#define SA_ThreadCreate(hthd, routine, arg, name) freertos_ThreadCreateWithStackSize(&hthd, routine, arg, 48*1024, name)

#define SA_SET_CURRENT_THREAD_NAME(name)

int SA_ThreadSuspend(SA_HTHREAD h);
int SA_ThreadResume(SA_HTHREAD h);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

