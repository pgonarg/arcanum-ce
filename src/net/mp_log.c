#include "mp_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE* s_log_file = NULL;

static const char* s_level_names[] = {
    "ERROR",
    "WARN ",
    "INFO ",
    "DEBUG",
    "TRACE",
};

void mp_log_init(void)
{
    s_log_file = fopen("arcanum_mp.log", "w");
    if (!s_log_file) {
        return;
    }

    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    fprintf(s_log_file,
            "=== Arcanum CE Multiplayer Log — Session started %s ===\n",
            timestamp);
    fflush(s_log_file);
}

void mp_log_shutdown(void)
{
    if (!s_log_file) {
        return;
    }

    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    fprintf(s_log_file,
            "=== Session ended %s ===\n",
            timestamp);
    fflush(s_log_file);
    fclose(s_log_file);
    s_log_file = NULL;
}

void mp_log_write(int level, const char* category, const char* fmt, ...)
{
    if (!s_log_file) {
        return;
    }

    if (level > MP_LOG_LEVEL) {
        return;
    }

    // Timestamp: HH:MM:SS
    time_t now = time(NULL);
    struct tm* t = localtime(&now);
    char timestamp[16];
    strftime(timestamp, sizeof(timestamp), "%H:%M:%S", t);

    // Format caller's message
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    fprintf(s_log_file, "[%s] [%s] [%-7s] %s\n",
            timestamp,
            s_level_names[level],
            category,
            msg);

    // Always flush on error; flush INFO and above eagerly too so the log
    // is readable while the game is running without a crash
    if (level <= MP_LOG_INFO) {
        fflush(s_log_file);
    }
}
