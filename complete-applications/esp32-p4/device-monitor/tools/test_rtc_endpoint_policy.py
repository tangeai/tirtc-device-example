"""Host-test the actual RTC configure/URL policy with ESP-IDF's URL parser.

Run with a native C compiler and --idf-path. No firmware, network, credentials
or serial port is used. SDK/RTOS side effects are stubbed; TLS is not emulated.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def c_function(source, signature):
    match = re.search(r"^" + re.escape(signature) + r"\n\{.*?^\}",
                      source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError(f"Function not found: {signature}")
    return match.group(0)


PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "http_parser.h"
typedef int esp_err_t;
enum { ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_STATE = 2, ESP_FAIL = 3 };
typedef struct {
    bool enabled;
    int default_session_mode;
    char service_endpoint[ENDPOINT_CAPACITY];
    char device_id[64];
    char client_id[64];
} tirtc_session_config_t;
static tirtc_session_config_t s_config;
static bool s_sdk_prepare_in_progress, s_start_in_progress, s_stop_in_progress;
static bool s_sdk_initialized, s_sdk_started;
static void *s_active_conn, *s_closing_conn;
static int s_session_mode, reset_count;
#define taskENTER_CRITICAL(...) ((void)0)
#define taskEXIT_CRITICAL(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static bool tirtc_connect_is_connecting(void) { return false; }
static bool tirtc_session_config_differs(const tirtc_session_config_t *a,
                                       const tirtc_session_config_t *b)
{ return memcmp(a, b, sizeof(*a)) != 0; }
static void tirtc_session_sync_stats_locked(void) {}
static bool tirtc_session_schedule_deferred_full_reset(void)
{ ++reset_count; return true; }
"""

CASES = r"""
static int count;
static void check(const char *url, bool enabled, bool accepted)
{
    tirtc_session_config_t candidate = { .enabled = enabled };
    snprintf(candidate.service_endpoint, sizeof(candidate.service_endpoint), "%s", url);
    tirtc_session_config_t input = candidate, saved = s_config;
    int saved_reset_count = reset_count;
    esp_err_t result = tirtc_session_configure(&candidate);
    assert(memcmp(&candidate, &input, sizeof(input)) == 0);
    assert(result == (accepted ? ESP_OK : ESP_ERR_INVALID_ARG));
    assert(memcmp(&s_config, accepted ? &input : &saved, sizeof(s_config)) == 0);
    if (!accepted) assert(reset_count == saved_reset_count);
    ++count;
}
int main(void)
{
    check("https://ep-tirtc.tange365.com", true, true);
    check("https://rtc.example:8443/api", true, true);
    check("https://rtc.example/path?env=prod", true, true);
    check("https://rtc.example", false, true);
    check("", false, true);
    check("", true, false);
    check("http://rtc.example", true, false);
    check("http://rtc.example", false, false);
    check("//rtc.example", true, false);
    check("wss://rtc.example", true, false);
    check("HTTPS://rtc.example", true, false);
    check("https://", true, false);
    check("https:///path", true, false);
    check("https://user:password@rtc.example/path", true, false);
    check("https://rtc.example/#fragment", true, false);
    check("https://rtc.example:0", true, false);
    check("https://rtc.example:65536", true, false);
    check("https://rtc.example\r\nInjected: value", true, false);
    check(" https://rtc.example", true, false);
    check("https://bad host/path", true, false);
    tirtc_session_config_t saved = s_config, unterminated = { .enabled = true };
    memset(unterminated.service_endpoint, 'x', sizeof(unterminated.service_endpoint));
    memcpy(unterminated.service_endpoint, "https://", 8);
    assert(tirtc_session_configure(&unterminated) == ESP_ERR_INVALID_ARG);
    assert(tirtc_session_configure(NULL) == ESP_ERR_INVALID_ARG);
    assert(memcmp(&s_config, &saved, sizeof(saved)) == 0);
    count += 2;
    s_sdk_initialized = true;
    check("https://rtc.example", true, true);
    assert(reset_count == 1);
    check("http://rtc.example", true, false);
    assert(reset_count == 1);
    printf("PASS: %d RTC endpoint cases; input preserved; rejection has no state/reset side effects\n", count);
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf-path", type=Path, required=True)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    session = root / "main/protocols/tirtc/tirtc_session.c"
    source = session.read_text(encoding="utf-8")
    header = session.with_suffix(".h").read_text(encoding="utf-8")
    capacity = re.search(r"#define TIRTC_SESSION_ENDPOINT_MAX_LEN\s+(\d+)", header).group(1)
    policy = c_function(source, "static bool tirtc_session_service_endpoint_is_valid(const tirtc_session_config_t *config)")
    configure = c_function(source, "esp_err_t tirtc_session_configure(const tirtc_session_config_t *config)")
    http_parser = args.idf_path / "components/http_parser"
    with tempfile.TemporaryDirectory(prefix=".rtc-endpoint-test-", dir=root) as temp:
        test_c = Path(temp) / "endpoint_test.c"
        executable = Path(temp) / ("endpoint_test.exe" if os.name == "nt" else "endpoint_test")
        test_c.write_text(f"#define ENDPOINT_CAPACITY {capacity}\n" + PREAMBLE + policy + configure + CASES,
                          encoding="utf-8")
        subprocess.run([args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-I", str(http_parser), str(test_c), str(http_parser / "http_parser.c"),
                        "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
