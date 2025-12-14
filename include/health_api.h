/**
 * @file health_api.h
 * @brief Health Check and Metrics API
 * 
 * Provides HTTP endpoints for:
 * - Health checks (liveness/readiness)
 * - Prometheus metrics export
 * - System status information
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#ifndef HEALTH_API_H
#define HEALTH_API_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Health Status
 * ============================================================================ */

typedef enum health_status {
    HEALTH_UP,          /* Component is healthy */
    HEALTH_DOWN,        /* Component is unhealthy */
    HEALTH_DEGRADED,    /* Component is working but impaired */
    HEALTH_UNKNOWN      /* Status cannot be determined */
} health_status_t;

/* ============================================================================
 * Component Health
 * ============================================================================ */

typedef struct component_health {
    const char *name;
    health_status_t status;
    const char *message;
    time_t last_check;
    double latency_ms;      /* Last check latency */
} component_health_t;

/* ============================================================================
 * System Metrics
 * ============================================================================ */

/**
 * @brief Counter metric (monotonically increasing)
 */
typedef struct metric_counter {
    const char *name;
    const char *help;
    const char *labels;     /* Optional labels in format: key1="val1",key2="val2" */
    uint64_t value;
} metric_counter_t;

/**
 * @brief Gauge metric (can go up and down)
 */
typedef struct metric_gauge {
    const char *name;
    const char *help;
    const char *labels;
    double value;
} metric_gauge_t;

/**
 * @brief Histogram bucket
 */
typedef struct histogram_bucket {
    double le;              /* Less than or equal */
    uint64_t count;
} histogram_bucket_t;

/**
 * @brief Histogram metric
 */
typedef struct metric_histogram {
    const char *name;
    const char *help;
    const char *labels;
    histogram_bucket_t *buckets;
    int bucket_count;
    uint64_t count;
    double sum;
} metric_histogram_t;

/* ============================================================================
 * Metrics Registry
 * ============================================================================ */

/**
 * @brief All system metrics
 */
typedef struct system_metrics {
    /* Process metrics */
    uint64_t process_start_time_seconds;
    double process_cpu_seconds_total;
    uint64_t process_resident_memory_bytes;
    uint64_t process_virtual_memory_bytes;
    uint64_t process_open_fds;
    
    /* Message processing */
    uint64_t messages_received_total;
    uint64_t messages_processed_total;
    uint64_t messages_failed_total;
    uint64_t messages_by_type[4];   /* ObjectDef, ValueUpdate, ObjectDelete, DeviceConfig */
    
    /* Processing latency histogram buckets (ms): 1, 5, 10, 25, 50, 100, 250, 500, 1000 */
    uint64_t processing_latency_bucket[10];
    uint64_t processing_latency_count;
    double processing_latency_sum;
    
    /* BACnet objects */
    uint64_t objects_total;
    uint64_t objects_by_type[10];   /* AI, AO, AV, BI, BO, BV, MSI, MSO, MSV */
    
    /* BACnet operations */
    uint64_t bacnet_read_requests_total;
    uint64_t bacnet_write_requests_total;
    uint64_t bacnet_cov_notifications_total;
    uint64_t bacnet_cov_subscriptions_active;
    
    /* Read latency histogram buckets (ms): 1, 5, 10, 25, 50, 100 */
    uint64_t read_latency_bucket[7];
    uint64_t read_latency_count;
    double read_latency_sum;
    
    /* Redis operations */
    uint64_t redis_commands_total;
    uint64_t redis_errors_total;
    uint64_t redis_reconnects_total;
    
    /* KurrentDB */
    uint64_t kurrentdb_events_received_total;
    uint64_t kurrentdb_events_acked_total;
    uint64_t kurrentdb_events_nacked_total;
    uint64_t kurrentdb_reconnects_total;
    int64_t kurrentdb_lag_events;   /* Events behind head */
    
    /* Errors */
    uint64_t errors_total;
    uint64_t errors_by_component[5]; /* Main, Redis, KurrentDB, BACnet, MessageHandler */
} system_metrics_t;

/* ============================================================================
 * Health API Configuration
 * ============================================================================ */

typedef struct health_api_config {
    int port;                   /* HTTP server port (default: 9090) */
    const char *bind_address;   /* Bind address (default: "0.0.0.0") */
    bool enable_pprof;          /* Enable pprof-like endpoints */
    int health_check_interval;  /* Background health check interval (seconds) */
} health_api_config_t;

/* ============================================================================
 * Health Check Callbacks
 * ============================================================================ */

/**
 * @brief Callback to check component health
 * @return Health status
 */
typedef health_status_t (*health_check_fn)(void *user_data, char *message, size_t message_size);

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * @brief Initialize health API server
 * @param config Configuration (NULL for defaults)
 * @return 0 on success, -1 on error
 */
int health_api_init(const health_api_config_t *config);

/**
 * @brief Shutdown health API server
 */
void health_api_shutdown(void);

/**
 * @brief Register a health check
 * @param name Component name
 * @param check_fn Health check function
 * @param user_data User data passed to check function
 * @return 0 on success, -1 on error
 */
int health_api_register_check(const char *name, health_check_fn check_fn, void *user_data);

/**
 * @brief Get global metrics pointer for updating
 * @return Pointer to metrics structure
 */
system_metrics_t* health_api_get_metrics(void);

/**
 * @brief Record a processing latency observation
 * @param latency_ms Latency in milliseconds
 */
void health_api_observe_processing_latency(double latency_ms);

/**
 * @brief Record a read latency observation
 * @param latency_ms Latency in milliseconds
 */
void health_api_observe_read_latency(double latency_ms);

/**
 * @brief Increment a counter metric
 * @param counter Pointer to counter
 */
void health_api_inc_counter(uint64_t *counter);

/**
 * @brief Add to a counter metric
 * @param counter Pointer to counter
 * @param value Value to add
 */
void health_api_add_counter(uint64_t *counter, uint64_t value);

/**
 * @brief Set a gauge metric
 * @param gauge Pointer to gauge
 * @param value New value
 */
void health_api_set_gauge(double *gauge, double value);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#define METRICS() health_api_get_metrics()

#define INC_MESSAGES_RECEIVED()     health_api_inc_counter(&METRICS()->messages_received_total)
#define INC_MESSAGES_PROCESSED()    health_api_inc_counter(&METRICS()->messages_processed_total)
#define INC_MESSAGES_FAILED()       health_api_inc_counter(&METRICS()->messages_failed_total)

#define INC_BACNET_READS()          health_api_inc_counter(&METRICS()->bacnet_read_requests_total)
#define INC_BACNET_WRITES()         health_api_inc_counter(&METRICS()->bacnet_write_requests_total)
#define INC_BACNET_COV_NOTIF()      health_api_inc_counter(&METRICS()->bacnet_cov_notifications_total)

#define INC_REDIS_COMMANDS()        health_api_inc_counter(&METRICS()->redis_commands_total)
#define INC_REDIS_ERRORS()          health_api_inc_counter(&METRICS()->redis_errors_total)

#define INC_KURRENTDB_RECEIVED()    health_api_inc_counter(&METRICS()->kurrentdb_events_received_total)
#define INC_KURRENTDB_ACKED()       health_api_inc_counter(&METRICS()->kurrentdb_events_acked_total)
#define INC_KURRENTDB_NACKED()      health_api_inc_counter(&METRICS()->kurrentdb_events_nacked_total)

#define INC_ERRORS()                health_api_inc_counter(&METRICS()->errors_total)

#define SET_OBJECTS_TOTAL(n)        METRICS()->objects_total = (n)
#define SET_COV_SUBSCRIPTIONS(n)    METRICS()->bacnet_cov_subscriptions_active = (n)
#define SET_KURRENTDB_LAG(n)        METRICS()->kurrentdb_lag_events = (n)

#define OBSERVE_PROCESSING_LATENCY(ms)  health_api_observe_processing_latency(ms)
#define OBSERVE_READ_LATENCY(ms)        health_api_observe_read_latency(ms)

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_API_H */
