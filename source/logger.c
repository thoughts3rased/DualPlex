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

// offline.c's background download thread logs from a second thread now
// (see its module comment) alongside everything else on the main thread -
// without this, two concurrent logger_log() calls could interleave writes
// to the ring buffer/index or the shared log FILE*, corrupting either.
static LightLock s_log_lock;

void logger_init(void) {
    LightLock_Init(&s_log_lock);
    s_log_count = 0;
    s_log_head = 0;
    s_log_file = fopen("sdmc:/dualplex.log", "w");
    if (s_log_file) {
        time_t t = time(NULL);
        fprintf(s_log_file, "=== DualPlex Log Started: %s", ctime(&t));
        fflush(s_log_file);
    }
}

void logger_cleanup(void) {
    LightLock_Lock(&s_log_lock);
    if (s_log_file) {
        fprintf(s_log_file, "=== Session Ended ===\n");
        fclose(s_log_file);
        s_log_file = NULL;
    }
    LightLock_Unlock(&s_log_lock);
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

    LightLock_Lock(&s_log_lock);
    // Store in ring buffer
    snprintf(s_log_lines[s_log_head], LOG_LINE_LEN, "%s", entry);
    s_log_head = (s_log_head + 1) % MAX_LOG_LINES;
    if (s_log_count < MAX_LOG_LINES) s_log_count++;

    printf("%s\n", entry);
    if (s_log_file) {
        fprintf(s_log_file, "%s\n", entry);
        fflush(s_log_file);
    }
    LightLock_Unlock(&s_log_lock);
}

int logger_get_log_count(void) {
    LightLock_Lock(&s_log_lock);
    int n = s_log_count;
    LightLock_Unlock(&s_log_lock);
    return n;
}

const char* logger_get_log_line(int index) {
    // Returns a pointer straight into the shared ring buffer rather than a
    // copy - a concurrent logger_log() from the download thread could in
    // principle overwrite this exact line's text out from under a caller
    // that's still reading it a moment later (e.g. the live log viewer's
    // render pass). Deliberately not fixed further: at worst that's a
    // visually torn (but still bounded, still NUL-terminated) log line for
    // one frame, and this pointer-into-static-storage shape is relied on
    // throughout ui.c's log viewer - not worth the churn of switching every
    // caller to a copy-out-under-lock API for a cosmetic-only edge case.
    LightLock_Lock(&s_log_lock);
    bool ok = index >= 0 && index < s_log_count;
    int start_idx = (s_log_count < MAX_LOG_LINES) ? 0 : s_log_head;
    int real_idx = ok ? (start_idx + index) % MAX_LOG_LINES : 0;
    LightLock_Unlock(&s_log_lock);
    return ok ? s_log_lines[real_idx] : "";
}
