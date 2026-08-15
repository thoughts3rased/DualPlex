/*
 * Silent stand-in for logger.c on the host. plex_api.c logs through the
 * LOG_INFO/LOG_WARN/LOG_ERROR macros (logger.h), which this satisfies without
 * needing 3DS's filesystem/console. Flip LOGGER_STUB_VERBOSE on to see the
 * log lines while debugging a failing test.
 */
#include "logger.h"
#include <stdio.h>
#include <stdarg.h>

#ifndef LOGGER_STUB_VERBOSE
#define LOGGER_STUB_VERBOSE 0
#endif

void logger_init(void) {}
void logger_cleanup(void) {}

void logger_log(const char* level, const char* fmt, ...) {
#if LOGGER_STUB_VERBOSE
    va_list args;
    printf("[%s] ", level ? level : "LOG");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
#else
    (void)level;
    (void)fmt;
#endif
}

int logger_get_log_count(void) { return 0; }
const char* logger_get_log_line(int index) { (void)index; return ""; }
