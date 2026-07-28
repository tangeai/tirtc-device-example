#pragma once

#include "tirtc_session.h"

const tirtc_session_media_ops_t *rtc_media_bridge_get_ops(void);
void *rtc_media_bridge_get_context(void);
