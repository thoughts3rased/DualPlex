#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdarg.h>

void logger_init(void);
void logger_cleanup(void);
void logger_log(const char* level, const char* fmt, ...);
int logger_get_log_count(void);
const char* logger_get_log_line(int index);

#define LOG_INFO(...) logger_log("INFO", __VA_ARGS__)
#define LOG_WARN(...) logger_log("WARN", __VA_ARGS__)
#define LOG_ERROR(...) logger_log("ERROR", __VA_ARGS__)

#endif // LOGGER_H
