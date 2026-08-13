#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/*
 * TiRTC v2.2.1 prints a peer-state trace and colored [tiRTC] lines directly
 * with printf(), bypassing both TiRtcLogSetCallback() and ESP-IDF tag levels.
 * Keep the compatibility shim narrow so unrelated stdout and fatal diagnostics
 * retain normal semantics.
 */
static const char *TIRTC_PEER_STATE_TRACE =
    "peer_connection state change e=%d\n";
static const char *TIRTC_CONSOLE_LOG_MARKER = "] [tiRTC]  ";

static bool is_tirtc_console_log(const char *line)
{
    if (line == NULL ||
        strncmp(line, "\x1b[1;3", 5) != 0 ||
        strlen(line) <= 7) {
        return false;
    }
    return line[5] >= '1' &&
           line[5] <= '4' &&
           line[6] == 'm' &&
           line[7] == '[' &&
           strstr(line, TIRTC_CONSOLE_LOG_MARKER) != NULL;
}

int __wrap_printf(const char *format, ...)
{
    if (format != NULL && strcmp(format, TIRTC_PEER_STATE_TRACE) == 0) {
        return 0;
    }

    va_list arguments;
    va_start(arguments, format);
    if (format != NULL && strcmp(format, "%s") == 0) {
        va_list probe;
        va_copy(probe, arguments);
        const char *line = va_arg(probe, const char *);
        va_end(probe);
        if (is_tirtc_console_log(line)) {
            va_end(arguments);
            return 0;
        }
    }
    int written = vprintf(format, arguments);
    va_end(arguments);
    return written;
}
