#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "logger.h"

#define MAX_LOG_LINES 60
#define LOG_LINE_LEN 256

static FILE* s_log_file = NULL;
static char s_log_lines[MAX_LOG_LINES][LOG_LINE_LEN];
static int s_log_count = 0;
static int s_log_head = 0;

void logger_init(void) {
    s_log_count = 0;
    s_log_head = 0;
    s_log_file = fopen("sdmc:/3ds-plex-client.log", "w");
    if (s_log_file) {
        time_t t = time(NULL);
        fprintf(s_log_file, "=== 3DS Plex Client Log Started: %s", ctime(&t));
        fflush(s_log_file);
    }
}

void logger_cleanup(void) {
    if (s_log_file) {
        fprintf(s_log_file, "=== Session Ended ===\n");
        fclose(s_log_file);
        s_log_file = NULL;
    }
}

void logger_log(const char* level, const char* fmt, ...) {
    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    
    // Format timestamp
    time_t rawtime = time(NULL);
    struct tm* timeinfo = localtime(&rawtime);
    char entry[LOG_LINE_LEN];
    if (timeinfo) {
        snprintf(entry, sizeof(entry), "[%02d:%02d:%02d] [%s] %s", 
            timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, level ? level : "LOG", msg);
    } else {
        snprintf(entry, sizeof(entry), "[%s] %s", level ? level : "LOG", msg);
    }
    
    // Store in ring buffer
    snprintf(s_log_lines[s_log_head], LOG_LINE_LEN, "%s", entry);
    s_log_head = (s_log_head + 1) % MAX_LOG_LINES;
    if (s_log_count < MAX_LOG_LINES) s_log_count++;

    printf("%s\n", entry);
    if (s_log_file) {
        fprintf(s_log_file, "%s\n", entry);
        fflush(s_log_file);
    }
}

int logger_get_log_count(void) {
    return s_log_count;
}

const char* logger_get_log_line(int index) {
    if (index < 0 || index >= s_log_count) return "";
    int start_idx = (s_log_count < MAX_LOG_LINES) ? 0 : s_log_head;
    int real_idx = (start_idx + index) % MAX_LOG_LINES;
    return s_log_lines[real_idx];
}
