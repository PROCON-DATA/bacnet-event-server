/**
 * @file logging.h
 * @brief Structured Logging System
 * 
 * Provides structured logging with multiple outputs (stdout, file, syslog),
 * log levels, and JSON formatting for log aggregation systems.
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#ifndef LOGGING_H
#define LOGGING_H

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Log Levels
 * ============================================================================ */

typedef enum log_level {
    LOG_LEVEL_TRACE = 0,
    LOG_LEVEL_DEBUG = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_WARN  = 3,
    LOG_LEVEL_ERROR = 4,
    LOG_LEVEL_FATAL = 5,
    LOG_LEVEL_OFF   = 6
} log_level_t;

/* ============================================================================
 * Log Output Targets
 * ============================================================================ */

typedef enum log_output {
    LOG_OUTPUT_STDOUT  = (1 << 0),
    LOG_OUTPUT_STDERR  = (1 << 1),
    LOG_OUTPUT_FILE    = (1 << 2),
    LOG_OUTPUT_SYSLOG  = (1 << 3),
    LOG_OUTPUT_CALLBACK = (1 << 4)
} log_output_t;

/* ============================================================================
 * Log Format
 * ============================================================================ */

typedef enum log_format {
    LOG_FORMAT_TEXT,    /* Human-readable text */
    LOG_FORMAT_JSON     /* JSON for log aggregation */
} log_format_t;

/* ============================================================================
 * Configuration
 * ============================================================================ */

/**
 * @brief Callback for custom log handling
 */
typedef void (*log_callback_fn)(
    log_level_t level,
    const char *component,
    const char *message,
    const char *file,
    int line,
    void *user_data
);

/**
 * @brief Logging configuration
 */
typedef struct log_config {
    log_level_t level;              /* Minimum log level */
    int outputs;                    /* Bitmask of log_output_t */
    log_format_t format;            /* Output format */
    
    /* File output */
    const char *file_path;          /* Log file path */
    size_t max_file_size;           /* Max file size before rotation (bytes) */
    int max_backup_files;           /* Number of rotated files to keep */
    
    /* Syslog */
    const char *syslog_ident;       /* Syslog identifier */
    int syslog_facility;            /* Syslog facility (LOG_LOCAL0, etc.) */
    
    /* Callback */
    log_callback_fn callback;
    void *callback_user_data;
    
    /* Options */
    bool include_timestamp;         /* Include timestamp in output */
    bool include_level;             /* Include log level */
    bool include_component;         /* Include component name */
    bool include_location;          /* Include file:line */
    bool colorize;                  /* ANSI colors for terminal */
} log_config_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize logging system
 * @param config Configuration (NULL for defaults)
 * @return 0 on success, -1 on error
 */
int log_init(const log_config_t *config);

/**
 * @brief Shutdown logging system
 */
void log_shutdown(void);

/**
 * @brief Get default configuration
 * @param config Output configuration
 */
void log_get_default_config(log_config_t *config);

/**
 * @brief Set log level at runtime
 * @param level New log level
 */
void log_set_level(log_level_t level);

/**
 * @brief Get current log level
 * @return Current log level
 */
log_level_t log_get_level(void);

/**
 * @brief Parse log level from string
 * @param str Level string (trace, debug, info, warn, error, fatal)
 * @return Log level, or LOG_LEVEL_INFO if invalid
 */
log_level_t log_level_from_string(const char *str);

/**
 * @brief Get log level name
 * @param level Log level
 * @return Level name string
 */
const char* log_level_to_string(log_level_t level);

/* ============================================================================
 * Logging Functions
 * ============================================================================ */

/**
 * @brief Log a message
 * @param level Log level
 * @param component Component name (e.g., "REDIS", "BACNET")
 * @param file Source file (use __FILE__)
 * @param line Source line (use __LINE__)
 * @param fmt Format string
 * @param ... Format arguments
 */
void log_write(
    log_level_t level,
    const char *component,
    const char *file,
    int line,
    const char *fmt,
    ...
) __attribute__((format(printf, 5, 6)));

/**
 * @brief Log with va_list
 */
void log_writev(
    log_level_t level,
    const char *component,
    const char *file,
    int line,
    const char *fmt,
    va_list args
);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#define LOG_TRACE(component, fmt, ...) \
    log_write(LOG_LEVEL_TRACE, component, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(component, fmt, ...) \
    log_write(LOG_LEVEL_DEBUG, component, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_INFO(component, fmt, ...) \
    log_write(LOG_LEVEL_INFO, component, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_WARN(component, fmt, ...) \
    log_write(LOG_LEVEL_WARN, component, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_ERROR(component, fmt, ...) \
    log_write(LOG_LEVEL_ERROR, component, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define LOG_FATAL(component, fmt, ...) \
    log_write(LOG_LEVEL_FATAL, component, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

/* Component-specific macros */
#define LOG_MAIN_TRACE(fmt, ...)   LOG_TRACE("MAIN", fmt, ##__VA_ARGS__)
#define LOG_MAIN_DEBUG(fmt, ...)   LOG_DEBUG("MAIN", fmt, ##__VA_ARGS__)
#define LOG_MAIN_INFO(fmt, ...)    LOG_INFO("MAIN", fmt, ##__VA_ARGS__)
#define LOG_MAIN_WARN(fmt, ...)    LOG_WARN("MAIN", fmt, ##__VA_ARGS__)
#define LOG_MAIN_ERROR(fmt, ...)   LOG_ERROR("MAIN", fmt, ##__VA_ARGS__)

#define LOG_REDIS_TRACE(fmt, ...)  LOG_TRACE("REDIS", fmt, ##__VA_ARGS__)
#define LOG_REDIS_DEBUG(fmt, ...)  LOG_DEBUG("REDIS", fmt, ##__VA_ARGS__)
#define LOG_REDIS_INFO(fmt, ...)   LOG_INFO("REDIS", fmt, ##__VA_ARGS__)
#define LOG_REDIS_WARN(fmt, ...)   LOG_WARN("REDIS", fmt, ##__VA_ARGS__)
#define LOG_REDIS_ERROR(fmt, ...)  LOG_ERROR("REDIS", fmt, ##__VA_ARGS__)

#define LOG_KURRENTDB_TRACE(fmt, ...) LOG_TRACE("KURRENTDB", fmt, ##__VA_ARGS__)
#define LOG_KURRENTDB_DEBUG(fmt, ...) LOG_DEBUG("KURRENTDB", fmt, ##__VA_ARGS__)
#define LOG_KURRENTDB_INFO(fmt, ...)  LOG_INFO("KURRENTDB", fmt, ##__VA_ARGS__)
#define LOG_KURRENTDB_WARN(fmt, ...)  LOG_WARN("KURRENTDB", fmt, ##__VA_ARGS__)
#define LOG_KURRENTDB_ERROR(fmt, ...) LOG_ERROR("KURRENTDB", fmt, ##__VA_ARGS__)

#define LOG_BACNET_TRACE(fmt, ...) LOG_TRACE("BACNET", fmt, ##__VA_ARGS__)
#define LOG_BACNET_DEBUG(fmt, ...) LOG_DEBUG("BACNET", fmt, ##__VA_ARGS__)
#define LOG_BACNET_INFO(fmt, ...)  LOG_INFO("BACNET", fmt, ##__VA_ARGS__)
#define LOG_BACNET_WARN(fmt, ...)  LOG_WARN("BACNET", fmt, ##__VA_ARGS__)
#define LOG_BACNET_ERROR(fmt, ...) LOG_ERROR("BACNET", fmt, ##__VA_ARGS__)

#define LOG_MSG_TRACE(fmt, ...)    LOG_TRACE("MSG", fmt, ##__VA_ARGS__)
#define LOG_MSG_DEBUG(fmt, ...)    LOG_DEBUG("MSG", fmt, ##__VA_ARGS__)
#define LOG_MSG_INFO(fmt, ...)     LOG_INFO("MSG", fmt, ##__VA_ARGS__)
#define LOG_MSG_WARN(fmt, ...)     LOG_WARN("MSG", fmt, ##__VA_ARGS__)
#define LOG_MSG_ERROR(fmt, ...)    LOG_ERROR("MSG", fmt, ##__VA_ARGS__)

#define LOG_HEALTH_TRACE(fmt, ...) LOG_TRACE("HEALTH", fmt, ##__VA_ARGS__)
#define LOG_HEALTH_DEBUG(fmt, ...) LOG_DEBUG("HEALTH", fmt, ##__VA_ARGS__)
#define LOG_HEALTH_INFO(fmt, ...)  LOG_INFO("HEALTH", fmt, ##__VA_ARGS__)
#define LOG_HEALTH_WARN(fmt, ...)  LOG_WARN("HEALTH", fmt, ##__VA_ARGS__)
#define LOG_HEALTH_ERROR(fmt, ...) LOG_ERROR("HEALTH", fmt, ##__VA_ARGS__)

/* ============================================================================
 * Structured Logging (Key-Value Pairs)
 * ============================================================================ */

/**
 * @brief Log with structured key-value data (JSON format recommended)
 */
void log_structured(
    log_level_t level,
    const char *component,
    const char *message,
    const char *json_data
);

#ifdef __cplusplus
}
#endif

#endif /* LOGGING_H */
