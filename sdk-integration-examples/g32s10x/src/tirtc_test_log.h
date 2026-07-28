#ifndef TIRTC_TEST_LOG_H
#define TIRTC_TEST_LOG_H

#include "tirtc_link.h"

/* Human-readable diagnostics used by the serial acceptance test. */
void tirtc_test_log_failure(const char *test, int error);
void tirtc_test_log_failure_detail(const char *test, int error,
                                   const char *meaning,
                                   const char *action);
const char *tirtc_test_state_description(tirtc_link_state_t state);

#endif
