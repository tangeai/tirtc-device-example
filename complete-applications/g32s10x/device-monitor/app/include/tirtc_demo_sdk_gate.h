#ifndef _TIRTC_DEMO_SDK_GATE_H
#define _TIRTC_DEMO_SDK_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TIRTC_DEMO_SDK_CONTROL_WAIT_MS 500U
#define TIRTC_DEMO_SDK_MEDIA_WAIT_MS 50U

int tirtc_demo_sdk_gate_init(void);
bool tirtc_demo_sdk_gate_take(uint32_t timeout_ms);
void tirtc_demo_sdk_gate_give(void);

#ifdef __cplusplus
}
#endif

#endif
