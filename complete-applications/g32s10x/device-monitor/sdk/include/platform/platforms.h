#ifndef __platforms_h__
#define __platforms_h__

#include "basedef.h"

/* Errors */
#define SA_QUEUE_ERR_TIMEOUT  0
#define SA_QUEUE_ERR_OTHER    -2


#if defined( WIN32 ) //Windows

#include "plat_win32.h"

#elif defined(__LINUX__) || defined(__ANDROID__) || defined(__MAC_OS__) || defined(__CYGWIN__)

#include "plat_linux.h"

#elif defined(__ALI_OS__)

#include "plat_alios.h"
//#include "net_lwip.h"

#elif defined(__UCOS2__)

#include "plat_ucos2.h"
#include "net_lwip.h"
#define __NO_FS__

#elif defined(__HI3861__) || defined(__AIW4211__)

//#include "plat_hi3861.h"
#include "plat_liteos.h"
#include "net_lwip.h"
#define __NO_FS__
#define __MCU__

#elif defined(__AIW62XX__)
#include "plat_aiw62xx.h"
#include "net_lwip.h"
#define __NO_FS__
#define __MCU__

#elif defined(__LITEOS__)

#include "plat_liteos.h"
#include "net_lwip.h"

#elif defined(__FREERTOS__) || defined(__ESP32S3__) || defined(__ESP32P4__) || defined(__G32S10X__) || defined(__EC71X__)
#define _ASSERT(x)
#include "plat_freertos.h"
#include "net_lwip.h"
#define __NO_FS__
//#ifdef __EC71X__
#define NO_TIMEZONE_SUPPORT 1
//#endif

#elif defined(__JL_AC57__)

#include "plat_jlac57.h"
#include "net_lwip.h"
#define __NO_FS__

#elif defined(__JL_AC79__)

#include "plat_jlac79.h"
#include "net_lwip.h"
#define __NO_FS__


#elif defined(__GP_CV4247__)

#include "plat_gpcv4247.h"
#include "net_lwip.h"
#define __NO_FS__

#elif defined(__TXW81X__)

//#include "plat_txw81x.h"
#include "plat_txw81x.2.h"
#include "net_lwip.h"
#define __NO_FS__

#elif defined(__TXW82X__)

#include "plat_txw82x.h"
#include "net_lwip.h"
#define __NO_FS__

#else

#error "Platform must be specified !"

#endif

#ifndef  __need_SA_platInit__
#define SA_platInit()
#endif

#ifdef __cplusplus
extern "C" {
#endif

char *Mx_strdup(const char *s);
char *Mx_strndup(const char *s, int len);
void *Mx_malloc(size_t size);
void *Mx_calloc(size_t nmemb, size_t size);
void *Mx_realloc(void *ptr, size_t size);
void Mx_free(void *ptr);


char *My_strdup(const char *s);
char *My_strndup(const char *s, int len);
void *My_malloc(size_t size);
void *My_calloc(size_t nmemb, size_t size);
void *My_realloc(void *ptr, size_t size);
void My_free(void *ptr);

#define Mj_malloc Mx_malloc
#define Mj_free   Mx_free
#define Mj_realloc Mx_realloc

#ifndef NO_TIMEZONE_SUPPORT
#define SA_localtime_r(t_ptr, tm_ptr) localtime_r(t_ptr, tm_ptr)
#define SA_mktime(tm_ptr) mktime(tm_ptr)
#else
extern long _tg_timezone_;
struct tm* SA_localtime_r(const time_t *t, struct tm *result);
time_t SA_mktime(const struct tm *tm);
#endif

#ifdef NO_GETTIMEOFDAY_SUPPORT
struct timeval;
struct timezone;
int SA_gettimeofday(struct timeval *tv, struct timezone *z);
int SA_settimeofday(const struct timeval *tv, const struct timezone *tz);
#else
#define SA_gettimeofday(tv, tz) gettimeofday(tv, tz)
#define SA_settimeofday(tv, tz) settimeofday(tv, tz)
#endif

#ifdef NO_STRTOK_R
char *strtok_r(char *s, const char *delim, char **saveptr);
#endif

#ifdef NO_LOCALTIME_R
struct tm *localtime_r(const time_t *timep, struct tm *result);
struct tm *gmtime_r(const time_t *timep, struct tm *result);
#endif

/*
 *  String functions
 */
#ifdef WIN32
#define SA_StrCaseCmp _stricmp
#define SA_StrNCaseCmp _strnicmp
#define SA_StrNCmp	_strncmp
#else
#define SA_StrCaseCmp strcasecmp
#define SA_StrNCaseCmp strncasecmp
#define SA_StrNCmp	strncmp
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
