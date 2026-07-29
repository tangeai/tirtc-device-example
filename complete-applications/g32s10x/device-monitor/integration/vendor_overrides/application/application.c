#ifdef CONFIG_APPLICATION_TIRTC_WIFI_LINK
#include "tirtc_link.h"
#endif

void applicetion_init(void *arg)
{
    (void)arg;
#ifdef CONFIG_APPLICATION_TIRTC_WIFI_LINK
    tirtc_link_service_init();
#endif
}
