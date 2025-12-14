/**
 * @file logging.c
 * @brief Structured Logging System Implementation
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef __linux__
#include <syslog.h>
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define LOG_BUFFER_SIZE 4096
#define LOG_TIMESTAMP_SIZE 32
#define LOG_MAX_MESSAGE_SIZE 2048

/* ANSI Color Codes */
#define ANSI_RESET   "\x1b[0m"
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_BLUE    "\x1b[34m"
#define ANSI_MAGENTA "\x1b[35m"
#define ANSI_CYAN    "\x1b[36m"
#define ANSI_WHITE   "\x1b[37m"
#define ANSI_BOLD    "\x1b[1m"

/* ============================================================================
 * Internal State
 * ============================================================================ */

static struct {
    log_config_t config;
    FILE *log_file;
    pthread_mutex_t mutex;
    bool initialized;
    size_t current_file_size;
} log_state = {
    .log_file = NULL,
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .initialized = false,
    .current_file_size = 0
};

/* Level names */
static const char *level_names[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"
};

/* Level colors */
static const char *level_colors[] = {
    ANSI_CYAN,    /* TRACE */
    ANSI_BLUE,    /* DEBUG */
    ANSI_GREEN,   /* INFO */
    ANSI_YELLOW,  /* WARN */
    ANSI_RED,     /* ERROR */
    ANSI_BOLD ANSI_RED, /* FATAL */
    ""            /* OFF */
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void get_timestamp(char *buffer, size_t size)
{
    struct timespec ts;
    struct tm tm_info;
    
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_info);
    
    int len = strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &tm_info);
    snprintf(buffer + len, size - len, ".%03ld", ts.tv_nsec / 1000000);
}

static void get_timestamp_iso8601(char *buffer, size_t size)
{
    struct timespec ts;
    struct tm tm_info;
    
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm_info);
    
    int len = strftime(buffer, size, "%Y-%m-%dT%H:%M:%S", &tm_info);
    snprintf(buffer + len, size - len, ".%03ldZ", ts.tv_nsec / 1000000);
}

static const char* get_filename(const char *path)
{
    const char *filename = strrchr(path, '/');
    return filename ? filename + 1 : path;
}

static void rotate_log_file(void)
{
    if (!log_state.log_file || !log_state.config.file_path) {
        return;
    }
    
    fclose(log_state.log_file);
    log_state.log_file = NULL;
    
    /* Rotate existing backup files */
    char old_path[512], new_path[512];
    
    for (int i = log_state.config.max_backup_files - 1; i >= 0; i--) {
        if (i == 0) {
            snprintf(old_path, sizeof(old_path), "%s", log_state.config.file_path);
        } else {
            snprintf(old_path, sizeof(old_path), "%s.%d", log_state.config.file_path, i);
        }
        snprintf(new_path, sizeof(new_path), "%s.%d", log_state.config.file_path, i + 1);
        
        rename(old_path, new_path);
    }
    
    /* Delete oldest if exceeds max */
    snprintf(old_path, sizeof(old_path), "%s.%d", 
             log_state.config.file_path, log_state.config.max_backup_files + 1);
    unlink(old_path);
    
    /* Reopen log file */
    log_state.log_file = fopen(log_state.config.file_path, "a");
    log_state.current_file_size = 0;
}

static void escape_json_string(const char *input, char *output, size_t output_size)
{
    size_t j = 0;
    for (size_t i = 0; input[i] && j < output_size - 2; i++) {
        switch (input[i]) {
            case '"':  output[j++] = '\\'; output[j++] = '"'; break;
            case '\\': output[j++] = '\\'; output[j++] = '\\'; break;
            case '\n': output[j++] = '\\'; output[j++] = 'n'; break;
            case '\r': output[j++] = '\\'; output[j++] = 'r'; break;
            case '\t': output[j++] = '\\'; output[j++] = 't'; break;
            default:   output[j++] = input[i]; break;
        }
    }
    output[j] = '\0';
}

/* ============================================================================
 * Public Functions
 * ============================================================================ */

void log_get_default_config(log_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->level = LOG_LEVEL_INFO;
    config->outputs = LOG_OUTPUT_STDOUT;
    config->format = LOG_FORMAT_TEXT;
    config->max_file_size = 10 * 1024 * 1024; /* 10 MB */
    config->max_backup_files = 5;
    config->include_timestamp = true;
    config->include_level = true;
    config->include_component = true;
    config->include_location = false;
    config->colorize = true;
}

int log_init(const log_config_t *config)
{
    pthread_mutex_lock(&log_state.mutex);
    
    if (log_state.initialized) {
        pthread_mutex_unlock(&log_state.mutex);
        return 0;
    }
    
    if (config) {
        memcpy(&log_state.config, config, sizeof(log_config_t));
    } else {
        log_get_default_config(&log_state.config);
    }
    
    /* Open log file if configured */
    if ((log_state.config.outputs & LOG_OUTPUT_FILE) && log_state.config.file_path) {
        log_state.log_file = fopen(log_state.config.file_path, "a");
        if (!log_state.log_file) {
            fprintf(stderr, "[LOG] Failed to open log file: %s\n", log_state.config.file_path);
            pthread_mutex_unlock(&log_state.mutex);
            return -1;
        }
        
        /* Get current file size */
        struct stat st;
        if (stat(log_state.config.file_path, &st) == 0) {
            log_state.current_file_size = st.st_size;
        }
    }
    
    /* Initialize syslog if configured */
#ifdef __linux__
    if (log_state.config.outputs & LOG_OUTPUT_SYSLOG) {
        openlog(
            log_state.config.syslog_ident ? log_state.config.syslog_ident : "bacnet-event-server",
            LOG_PID | LOG_NDELAY,
            log_state.config.syslog_facility ? log_state.config.syslog_facility : LOG_LOCAL0
        );
    }
#endif
    
    log_state.initialized = true;
    pthread_mutex_unlock(&log_state.mutex);
    
    LOG_INFO("LOG", "Logging system initialized (level=%s)", 
             log_level_to_string(log_state.config.level));
    
    return 0;
}

void log_shutdown(void)
{
    pthread_mutex_lock(&log_state.mutex);
    
    if (!log_state.initialized) {
        pthread_mutex_unlock(&log_state.mutex);
        return;
    }
    
    if (log_state.log_file) {
        fclose(log_state.log_file);
        log_state.log_file = NULL;
    }
    
#ifdef __linux__
    if (log_state.config.outputs & LOG_OUTPUT_SYSLOG) {
        closelog();
    }
#endif
    
    log_state.initialized = false;
    pthread_mutex_unlock(&log_state.mutex);
}

void log_set_level(log_level_t level)
{
    pthread_mutex_lock(&log_state.mutex);
    log_state.config.level = level;
    pthread_mutex_unlock(&log_state.mutex);
}

log_level_t log_get_level(void)
{
    return log_state.config.level;
}

log_level_t log_level_from_string(const char *str)
{
    if (!str) return LOG_LEVEL_INFO;
    
    if (strcasecmp(str, "trace") == 0) return LOG_LEVEL_TRACE;
    if (strcasecmp(str, "debug") == 0) return LOG_LEVEL_DEBUG;
    if (strcasecmp(str, "info") == 0)  return LOG_LEVEL_INFO;
    if (strcasecmp(str, "warn") == 0 || strcasecmp(str, "warning") == 0) return LOG_LEVEL_WARN;
    if (strcasecmp(str, "error") == 0) return LOG_LEVEL_ERROR;
    if (strcasecmp(str, "fatal") == 0) return LOG_LEVEL_FATAL;
    if (strcasecmp(str, "off") == 0)   return LOG_LEVEL_OFF;
    
    return LOG_LEVEL_INFO;
}

const char* log_level_to_string(log_level_t level)
{
    if (level >= 0 && level <= LOG_LEVEL_OFF) {
        return level_names[level];
    }
    return "UNKNOWN";
}

void log_writev(
    log_level_t level,
    const char *component,
    const char *file,
    int line,
    const char *fmt,
    va_list args)
{
    if (level < log_state.config.level) {
        return;
    }
    
    char message[LOG_MAX_MESSAGE_SIZE];
    vsnprintf(message, sizeof(message), fmt, args);
    
    char timestamp[LOG_TIMESTAMP_SIZE];
    char buffer[LOG_BUFFER_SIZE];
    
    pthread_mutex_lock(&log_state.mutex);
    
    if (log_state.config.format == LOG_FORMAT_JSON) {
        /* JSON format */
        get_timestamp_iso8601(timestamp, sizeof(timestamp));
        
        char escaped_msg[LOG_MAX_MESSAGE_SIZE];
        escape_json_string(message, escaped_msg, sizeof(escaped_msg));
        
        snprintf(buffer, sizeof(buffer),
            "{\"timestamp\":\"%s\",\"level\":\"%s\",\"component\":\"%s\","
            "\"file\":\"%s\",\"line\":%d,\"message\":\"%s\"}\n",
            timestamp,
            level_names[level],
            component ? component : "",
            get_filename(file),
            line,
            escaped_msg
        );
    } else {
        /* Text format */
        get_timestamp(timestamp, sizeof(timestamp));
        
        int offset = 0;
        
        if (log_state.config.include_timestamp) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%s ", timestamp);
        }
        
        if (log_state.config.include_level) {
            if (log_state.config.colorize && (log_state.config.outputs & (LOG_OUTPUT_STDOUT | LOG_OUTPUT_STDERR))) {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset, 
                    "%s%-5s%s ", level_colors[level], level_names[level], ANSI_RESET);
            } else {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset, 
                    "%-5s ", level_names[level]);
            }
        }
        
        if (log_state.config.include_component && component) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, "[%s] ", component);
        }
        
        if (log_state.config.include_location) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset, 
                "(%s:%d) ", get_filename(file), line);
        }
        
        snprintf(buffer + offset, sizeof(buffer) - offset, "%s\n", message);
    }
    
    /* Output to configured destinations */
    if (log_state.config.outputs & LOG_OUTPUT_STDOUT) {
        fputs(buffer, stdout);
        fflush(stdout);
    }
    
    if (log_state.config.outputs & LOG_OUTPUT_STDERR) {
        fputs(buffer, stderr);
        fflush(stderr);
    }
    
    if ((log_state.config.outputs & LOG_OUTPUT_FILE) && log_state.log_file) {
        /* For file output, strip ANSI codes */
        char *clean_buffer = buffer;
        if (log_state.config.format == LOG_FORMAT_TEXT && log_state.config.colorize) {
            /* Simple ANSI stripping for file */
            static char file_buffer[LOG_BUFFER_SIZE];
            char *src = buffer, *dst = file_buffer;
            while (*src && dst < file_buffer + sizeof(file_buffer) - 1) {
                if (*src == '\x1b') {
                    while (*src && *src != 'm') src++;
                    if (*src) src++;
                } else {
                    *dst++ = *src++;
                }
            }
            *dst = '\0';
            clean_buffer = file_buffer;
        }
        
        size_t written = fwrite(clean_buffer, 1, strlen(clean_buffer), log_state.log_file);
        fflush(log_state.log_file);
        log_state.current_file_size += written;
        
        /* Check for rotation */
        if (log_state.config.max_file_size > 0 && 
            log_state.current_file_size >= log_state.config.max_file_size) {
            rotate_log_file();
        }
    }
    
#ifdef __linux__
    if (log_state.config.outputs & LOG_OUTPUT_SYSLOG) {
        int syslog_level;
        switch (level) {
            case LOG_LEVEL_TRACE:
            case LOG_LEVEL_DEBUG: syslog_level = LOG_DEBUG; break;
            case LOG_LEVEL_INFO:  syslog_level = LOG_INFO; break;
            case LOG_LEVEL_WARN:  syslog_level = LOG_WARNING; break;
            case LOG_LEVEL_ERROR: syslog_level = LOG_ERR; break;
            case LOG_LEVEL_FATAL: syslog_level = LOG_CRIT; break;
            default:              syslog_level = LOG_INFO; break;
        }
        syslog(syslog_level, "[%s] %s", component ? component : "", message);
    }
#endif
    
    if ((log_state.config.outputs & LOG_OUTPUT_CALLBACK) && log_state.config.callback) {
        log_state.config.callback(level, component, message, file, line, 
                                  log_state.config.callback_user_data);
    }
    
    pthread_mutex_unlock(&log_state.mutex);
}

void log_write(
    log_level_t level,
    const char *component,
    const char *file,
    int line,
    const char *fmt,
    ...)
{
    va_list args;
    va_start(args, fmt);
    log_writev(level, component, file, line, fmt, args);
    va_end(args);
}

void log_structured(
    log_level_t level,
    const char *component,
    const char *message,
    const char *json_data)
{
    if (level < log_state.config.level) {
        return;
    }
    
    char timestamp[LOG_TIMESTAMP_SIZE];
    char buffer[LOG_BUFFER_SIZE];
    
    get_timestamp_iso8601(timestamp, sizeof(timestamp));
    
    char escaped_msg[LOG_MAX_MESSAGE_SIZE];
    escape_json_string(message, escaped_msg, sizeof(escaped_msg));
    
    pthread_mutex_lock(&log_state.mutex);
    
    if (json_data && json_data[0]) {
        snprintf(buffer, sizeof(buffer),
            "{\"timestamp\":\"%s\",\"level\":\"%s\",\"component\":\"%s\","
            "\"message\":\"%s\",\"data\":%s}\n",
            timestamp,
            level_names[level],
            component ? component : "",
            escaped_msg,
            json_data
        );
    } else {
        snprintf(buffer, sizeof(buffer),
            "{\"timestamp\":\"%s\",\"level\":\"%s\",\"component\":\"%s\","
            "\"message\":\"%s\"}\n",
            timestamp,
            level_names[level],
            component ? component : "",
            escaped_msg
        );
    }
    
    /* Output to configured destinations */
    if (log_state.config.outputs & LOG_OUTPUT_STDOUT) {
        fputs(buffer, stdout);
        fflush(stdout);
    }
    
    if ((log_state.config.outputs & LOG_OUTPUT_FILE) && log_state.log_file) {
        size_t written = fwrite(buffer, 1, strlen(buffer), log_state.log_file);
        fflush(log_state.log_file);
        log_state.current_file_size += written;
    }
    
    pthread_mutex_unlock(&log_state.mutex);
}
