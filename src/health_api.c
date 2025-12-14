/**
 * @file health_api.c
 * @brief Health Check and Metrics API Implementation
 * 
 * Implements a minimal HTTP server for health checks and Prometheus metrics.
 * 
 * Endpoints:
 * - GET /health         - Overall health status (JSON)
 * - GET /health/live    - Liveness probe (200 if running)
 * - GET /health/ready   - Readiness probe (200 if all components ready)
 * - GET /metrics        - Prometheus metrics format
 * - GET /status         - Detailed system status (JSON)
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#include "health_api.h"
#include "logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define MAX_HEALTH_CHECKS 16
#define HTTP_BUFFER_SIZE 8192
#define METRICS_BUFFER_SIZE 32768
#define MAX_CONNECTIONS 10

/* Processing latency histogram buckets (ms) */
static const double PROCESSING_LATENCY_BUCKETS[] = {1, 5, 10, 25, 50, 100, 250, 500, 1000, INFINITY};
#define NUM_PROCESSING_BUCKETS 10

/* Read latency histogram buckets (ms) */
static const double READ_LATENCY_BUCKETS[] = {1, 5, 10, 25, 50, 100, INFINITY};
#define NUM_READ_BUCKETS 7

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

typedef struct registered_check {
    char name[64];
    health_check_fn check_fn;
    void *user_data;
    health_status_t last_status;
    char last_message[256];
    time_t last_check_time;
} registered_check_t;

static struct {
    health_api_config_t config;
    system_metrics_t metrics;
    registered_check_t checks[MAX_HEALTH_CHECKS];
    int check_count;
    int server_socket;
    pthread_t server_thread;
    pthread_mutex_t mutex;
    bool running;
    bool initialized;
    time_t start_time;
} api_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .server_socket = -1,
    .running = false,
    .initialized = false
};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static const char* health_status_string(health_status_t status)
{
    switch (status) {
        case HEALTH_UP:       return "UP";
        case HEALTH_DOWN:     return "DOWN";
        case HEALTH_DEGRADED: return "DEGRADED";
        default:              return "UNKNOWN";
    }
}

static void update_process_metrics(void)
{
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        api_state.metrics.process_cpu_seconds_total = 
            usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
            (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
    }
    
    /* Read memory from /proc/self/statm on Linux */
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        unsigned long vsize, rss;
        if (fscanf(f, "%lu %lu", &vsize, &rss) == 2) {
            long page_size = sysconf(_SC_PAGESIZE);
            api_state.metrics.process_virtual_memory_bytes = vsize * page_size;
            api_state.metrics.process_resident_memory_bytes = rss * page_size;
        }
        fclose(f);
    }
    
    /* Count open file descriptors */
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd");
    int fd_count = 0;
    DIR *dir = opendir(path);
    if (dir) {
        while (readdir(dir)) fd_count++;
        closedir(dir);
        api_state.metrics.process_open_fds = fd_count - 2; /* Exclude . and .. */
    }
}

static int find_histogram_bucket(const double *buckets, int count, double value)
{
    for (int i = 0; i < count; i++) {
        if (value <= buckets[i]) {
            return i;
        }
    }
    return count - 1;
}

/* ============================================================================
 * HTTP Response Helpers
 * ============================================================================ */

static void send_response(int client_fd, int status_code, const char *content_type, 
                          const char *body, size_t body_len)
{
    char header[512];
    const char *status_text;
    
    switch (status_code) {
        case 200: status_text = "OK"; break;
        case 404: status_text = "Not Found"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 503: status_text = "Service Unavailable"; break;
        default:  status_text = "Unknown"; break;
    }
    
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len);
    
    send(client_fd, header, header_len, 0);
    if (body && body_len > 0) {
        send(client_fd, body, body_len, 0);
    }
}

static void send_json(int client_fd, int status_code, const char *json)
{
    send_response(client_fd, status_code, "application/json", json, strlen(json));
}

static void send_text(int client_fd, int status_code, const char *text)
{
    send_response(client_fd, status_code, "text/plain; charset=utf-8", text, strlen(text));
}

/* ============================================================================
 * Endpoint Handlers
 * ============================================================================ */

static void handle_health(int client_fd)
{
    char response[4096];
    int offset = 0;
    bool all_healthy = true;
    
    pthread_mutex_lock(&api_state.mutex);
    
    offset += snprintf(response + offset, sizeof(response) - offset,
        "{\"status\":\"");
    
    /* Run all health checks */
    for (int i = 0; i < api_state.check_count; i++) {
        registered_check_t *check = &api_state.checks[i];
        if (check->check_fn) {
            check->last_status = check->check_fn(check->user_data, 
                check->last_message, sizeof(check->last_message));
            check->last_check_time = time(NULL);
            
            if (check->last_status != HEALTH_UP) {
                all_healthy = false;
            }
        }
    }
    
    offset += snprintf(response + offset, sizeof(response) - offset,
        "%s\",\"components\":{", all_healthy ? "UP" : "DOWN");
    
    for (int i = 0; i < api_state.check_count; i++) {
        registered_check_t *check = &api_state.checks[i];
        if (i > 0) {
            offset += snprintf(response + offset, sizeof(response) - offset, ",");
        }
        offset += snprintf(response + offset, sizeof(response) - offset,
            "\"%s\":{\"status\":\"%s\",\"message\":\"%s\"}",
            check->name,
            health_status_string(check->last_status),
            check->last_message[0] ? check->last_message : "OK");
    }
    
    offset += snprintf(response + offset, sizeof(response) - offset, "}}");
    
    pthread_mutex_unlock(&api_state.mutex);
    
    send_json(client_fd, all_healthy ? 200 : 503, response);
}

static void handle_health_live(int client_fd)
{
    send_json(client_fd, 200, "{\"status\":\"UP\"}");
}

static void handle_health_ready(int client_fd)
{
    bool ready = true;
    
    pthread_mutex_lock(&api_state.mutex);
    for (int i = 0; i < api_state.check_count; i++) {
        registered_check_t *check = &api_state.checks[i];
        if (check->check_fn) {
            health_status_t status = check->check_fn(check->user_data, NULL, 0);
            if (status != HEALTH_UP) {
                ready = false;
                break;
            }
        }
    }
    pthread_mutex_unlock(&api_state.mutex);
    
    if (ready) {
        send_json(client_fd, 200, "{\"status\":\"UP\"}");
    } else {
        send_json(client_fd, 503, "{\"status\":\"DOWN\"}");
    }
}

static void handle_metrics(int client_fd)
{
    char *buffer = malloc(METRICS_BUFFER_SIZE);
    if (!buffer) {
        send_text(client_fd, 500, "Memory allocation failed");
        return;
    }
    
    int offset = 0;
    
    pthread_mutex_lock(&api_state.mutex);
    update_process_metrics();
    system_metrics_t *m = &api_state.metrics;
    
    /* Process metrics */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP process_start_time_seconds Start time of the process since unix epoch in seconds.\n"
        "# TYPE process_start_time_seconds gauge\n"
        "process_start_time_seconds %lu\n\n",
        m->process_start_time_seconds);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP process_cpu_seconds_total Total user and system CPU time spent in seconds.\n"
        "# TYPE process_cpu_seconds_total counter\n"
        "process_cpu_seconds_total %.6f\n\n",
        m->process_cpu_seconds_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP process_resident_memory_bytes Resident memory size in bytes.\n"
        "# TYPE process_resident_memory_bytes gauge\n"
        "process_resident_memory_bytes %lu\n\n",
        m->process_resident_memory_bytes);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP process_virtual_memory_bytes Virtual memory size in bytes.\n"
        "# TYPE process_virtual_memory_bytes gauge\n"
        "process_virtual_memory_bytes %lu\n\n",
        m->process_virtual_memory_bytes);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP process_open_fds Number of open file descriptors.\n"
        "# TYPE process_open_fds gauge\n"
        "process_open_fds %lu\n\n",
        m->process_open_fds);
    
    /* Message metrics */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_messages_received_total Total number of messages received from KurrentDB.\n"
        "# TYPE bacnet_messages_received_total counter\n"
        "bacnet_messages_received_total %lu\n\n",
        m->messages_received_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_messages_processed_total Total number of messages successfully processed.\n"
        "# TYPE bacnet_messages_processed_total counter\n"
        "bacnet_messages_processed_total %lu\n\n",
        m->messages_processed_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_messages_failed_total Total number of messages that failed processing.\n"
        "# TYPE bacnet_messages_failed_total counter\n"
        "bacnet_messages_failed_total %lu\n\n",
        m->messages_failed_total);
    
    /* Processing latency histogram */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_message_processing_seconds Time spent processing messages.\n"
        "# TYPE bacnet_message_processing_seconds histogram\n");
    
    uint64_t cumulative = 0;
    for (int i = 0; i < NUM_PROCESSING_BUCKETS; i++) {
        cumulative += m->processing_latency_bucket[i];
        if (PROCESSING_LATENCY_BUCKETS[i] == INFINITY) {
            offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
                "bacnet_message_processing_seconds_bucket{le=\"+Inf\"} %lu\n", cumulative);
        } else {
            offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
                "bacnet_message_processing_seconds_bucket{le=\"%.3f\"} %lu\n",
                PROCESSING_LATENCY_BUCKETS[i] / 1000.0, cumulative);
        }
    }
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "bacnet_message_processing_seconds_sum %.6f\n"
        "bacnet_message_processing_seconds_count %lu\n\n",
        m->processing_latency_sum / 1000.0, m->processing_latency_count);
    
    /* BACnet objects */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_objects_total Total number of BACnet objects.\n"
        "# TYPE bacnet_objects_total gauge\n"
        "bacnet_objects_total %lu\n\n",
        m->objects_total);
    
    /* BACnet operations */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_read_requests_total Total number of BACnet read requests.\n"
        "# TYPE bacnet_read_requests_total counter\n"
        "bacnet_read_requests_total %lu\n\n",
        m->bacnet_read_requests_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_write_requests_total Total number of BACnet write requests.\n"
        "# TYPE bacnet_write_requests_total counter\n"
        "bacnet_write_requests_total %lu\n\n",
        m->bacnet_write_requests_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_cov_notifications_total Total number of COV notifications sent.\n"
        "# TYPE bacnet_cov_notifications_total counter\n"
        "bacnet_cov_notifications_total %lu\n\n",
        m->bacnet_cov_notifications_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_cov_subscriptions_active Number of active COV subscriptions.\n"
        "# TYPE bacnet_cov_subscriptions_active gauge\n"
        "bacnet_cov_subscriptions_active %lu\n\n",
        m->bacnet_cov_subscriptions_active);
    
    /* Read latency histogram */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_read_latency_seconds Time spent handling read requests.\n"
        "# TYPE bacnet_read_latency_seconds histogram\n");
    
    cumulative = 0;
    for (int i = 0; i < NUM_READ_BUCKETS; i++) {
        cumulative += m->read_latency_bucket[i];
        if (READ_LATENCY_BUCKETS[i] == INFINITY) {
            offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
                "bacnet_read_latency_seconds_bucket{le=\"+Inf\"} %lu\n", cumulative);
        } else {
            offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
                "bacnet_read_latency_seconds_bucket{le=\"%.3f\"} %lu\n",
                READ_LATENCY_BUCKETS[i] / 1000.0, cumulative);
        }
    }
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "bacnet_read_latency_seconds_sum %.6f\n"
        "bacnet_read_latency_seconds_count %lu\n\n",
        m->read_latency_sum / 1000.0, m->read_latency_count);
    
    /* Redis metrics */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_redis_commands_total Total number of Redis commands executed.\n"
        "# TYPE bacnet_redis_commands_total counter\n"
        "bacnet_redis_commands_total %lu\n\n",
        m->redis_commands_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_redis_errors_total Total number of Redis errors.\n"
        "# TYPE bacnet_redis_errors_total counter\n"
        "bacnet_redis_errors_total %lu\n\n",
        m->redis_errors_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_redis_reconnects_total Total number of Redis reconnections.\n"
        "# TYPE bacnet_redis_reconnects_total counter\n"
        "bacnet_redis_reconnects_total %lu\n\n",
        m->redis_reconnects_total);
    
    /* KurrentDB metrics */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_kurrentdb_events_received_total Total events received from KurrentDB.\n"
        "# TYPE bacnet_kurrentdb_events_received_total counter\n"
        "bacnet_kurrentdb_events_received_total %lu\n\n",
        m->kurrentdb_events_received_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_kurrentdb_events_acked_total Total events acknowledged.\n"
        "# TYPE bacnet_kurrentdb_events_acked_total counter\n"
        "bacnet_kurrentdb_events_acked_total %lu\n\n",
        m->kurrentdb_events_acked_total);
    
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_kurrentdb_lag_events Number of events behind stream head.\n"
        "# TYPE bacnet_kurrentdb_lag_events gauge\n"
        "bacnet_kurrentdb_lag_events %ld\n\n",
        m->kurrentdb_lag_events);
    
    /* Errors */
    offset += snprintf(buffer + offset, METRICS_BUFFER_SIZE - offset,
        "# HELP bacnet_errors_total Total number of errors.\n"
        "# TYPE bacnet_errors_total counter\n"
        "bacnet_errors_total %lu\n",
        m->errors_total);
    
    pthread_mutex_unlock(&api_state.mutex);
    
    send_response(client_fd, 200, "text/plain; version=0.0.4; charset=utf-8", buffer, offset);
    free(buffer);
}

static void handle_status(int client_fd)
{
    char response[4096];
    int offset = 0;
    
    pthread_mutex_lock(&api_state.mutex);
    update_process_metrics();
    system_metrics_t *m = &api_state.metrics;
    
    time_t uptime = time(NULL) - api_state.start_time;
    
    offset += snprintf(response + offset, sizeof(response) - offset,
        "{"
        "\"version\":\"1.0.0\","
        "\"uptime_seconds\":%ld,"
        "\"messages\":{\"received\":%lu,\"processed\":%lu,\"failed\":%lu},"
        "\"objects\":{\"total\":%lu},"
        "\"bacnet\":{\"reads\":%lu,\"writes\":%lu,\"cov_notifications\":%lu,\"cov_subscriptions\":%lu},"
        "\"redis\":{\"commands\":%lu,\"errors\":%lu,\"reconnects\":%lu},"
        "\"kurrentdb\":{\"events_received\":%lu,\"events_acked\":%lu,\"lag\":%ld},"
        "\"process\":{\"cpu_seconds\":%.2f,\"memory_bytes\":%lu,\"open_fds\":%lu}"
        "}",
        uptime,
        m->messages_received_total, m->messages_processed_total, m->messages_failed_total,
        m->objects_total,
        m->bacnet_read_requests_total, m->bacnet_write_requests_total,
        m->bacnet_cov_notifications_total, m->bacnet_cov_subscriptions_active,
        m->redis_commands_total, m->redis_errors_total, m->redis_reconnects_total,
        m->kurrentdb_events_received_total, m->kurrentdb_events_acked_total, m->kurrentdb_lag_events,
        m->process_cpu_seconds_total, m->process_resident_memory_bytes, m->process_open_fds);
    
    pthread_mutex_unlock(&api_state.mutex);
    
    send_json(client_fd, 200, response);
}

/* ============================================================================
 * HTTP Server
 * ============================================================================ */

static void handle_request(int client_fd)
{
    char buffer[HTTP_BUFFER_SIZE];
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        return;
    }
    buffer[bytes_read] = '\0';
    
    /* Parse request line */
    char method[16], path[256];
    if (sscanf(buffer, "%15s %255s", method, path) != 2) {
        send_text(client_fd, 400, "Bad Request");
        return;
    }
    
    /* Only support GET */
    if (strcmp(method, "GET") != 0) {
        send_text(client_fd, 405, "Method Not Allowed");
        return;
    }
    
    /* Route request */
    if (strcmp(path, "/health") == 0) {
        handle_health(client_fd);
    } else if (strcmp(path, "/health/live") == 0 || strcmp(path, "/healthz") == 0) {
        handle_health_live(client_fd);
    } else if (strcmp(path, "/health/ready") == 0 || strcmp(path, "/readyz") == 0) {
        handle_health_ready(client_fd);
    } else if (strcmp(path, "/metrics") == 0) {
        handle_metrics(client_fd);
    } else if (strcmp(path, "/status") == 0) {
        handle_status(client_fd);
    } else {
        send_text(client_fd, 404, "Not Found");
    }
}

static void* server_thread_fn(void *arg)
{
    (void)arg;
    
    LOG_HEALTH_INFO("Health API server started on port %d", api_state.config.port);
    
    while (api_state.running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(api_state.server_socket, 
                               (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (api_state.running) {
                LOG_HEALTH_ERROR("Accept failed: %s", strerror(errno));
            }
            continue;
        }
        
        /* Set socket timeout */
        struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        
        handle_request(client_fd);
        close(client_fd);
    }
    
    LOG_HEALTH_INFO("Health API server stopped");
    return NULL;
}

/* ============================================================================
 * Public Functions
 * ============================================================================ */

int health_api_init(const health_api_config_t *config)
{
    pthread_mutex_lock(&api_state.mutex);
    
    if (api_state.initialized) {
        pthread_mutex_unlock(&api_state.mutex);
        return 0;
    }
    
    /* Set defaults */
    if (config) {
        memcpy(&api_state.config, config, sizeof(health_api_config_t));
    } else {
        api_state.config.port = 9090;
        api_state.config.bind_address = "0.0.0.0";
        api_state.config.enable_pprof = false;
        api_state.config.health_check_interval = 30;
    }
    
    /* Initialize metrics */
    memset(&api_state.metrics, 0, sizeof(system_metrics_t));
    api_state.start_time = time(NULL);
    api_state.metrics.process_start_time_seconds = api_state.start_time;
    
    /* Create server socket */
    api_state.server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (api_state.server_socket < 0) {
        LOG_HEALTH_ERROR("Failed to create socket: %s", strerror(errno));
        pthread_mutex_unlock(&api_state.mutex);
        return -1;
    }
    
    /* Allow address reuse */
    int opt = 1;
    setsockopt(api_state.server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    /* Bind */
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(api_state.config.port),
        .sin_addr.s_addr = inet_addr(api_state.config.bind_address ? 
                                      api_state.config.bind_address : "0.0.0.0")
    };
    
    if (bind(api_state.server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_HEALTH_ERROR("Failed to bind to port %d: %s", api_state.config.port, strerror(errno));
        close(api_state.server_socket);
        api_state.server_socket = -1;
        pthread_mutex_unlock(&api_state.mutex);
        return -1;
    }
    
    /* Listen */
    if (listen(api_state.server_socket, MAX_CONNECTIONS) < 0) {
        LOG_HEALTH_ERROR("Failed to listen: %s", strerror(errno));
        close(api_state.server_socket);
        api_state.server_socket = -1;
        pthread_mutex_unlock(&api_state.mutex);
        return -1;
    }
    
    /* Start server thread */
    api_state.running = true;
    if (pthread_create(&api_state.server_thread, NULL, server_thread_fn, NULL) != 0) {
        LOG_HEALTH_ERROR("Failed to create server thread");
        close(api_state.server_socket);
        api_state.server_socket = -1;
        api_state.running = false;
        pthread_mutex_unlock(&api_state.mutex);
        return -1;
    }
    
    api_state.initialized = true;
    pthread_mutex_unlock(&api_state.mutex);
    
    return 0;
}

void health_api_shutdown(void)
{
    pthread_mutex_lock(&api_state.mutex);
    
    if (!api_state.initialized) {
        pthread_mutex_unlock(&api_state.mutex);
        return;
    }
    
    api_state.running = false;
    
    if (api_state.server_socket >= 0) {
        shutdown(api_state.server_socket, SHUT_RDWR);
        close(api_state.server_socket);
        api_state.server_socket = -1;
    }
    
    pthread_mutex_unlock(&api_state.mutex);
    
    pthread_join(api_state.server_thread, NULL);
    
    pthread_mutex_lock(&api_state.mutex);
    api_state.initialized = false;
    pthread_mutex_unlock(&api_state.mutex);
}

int health_api_register_check(const char *name, health_check_fn check_fn, void *user_data)
{
    pthread_mutex_lock(&api_state.mutex);
    
    if (api_state.check_count >= MAX_HEALTH_CHECKS) {
        pthread_mutex_unlock(&api_state.mutex);
        return -1;
    }
    
    registered_check_t *check = &api_state.checks[api_state.check_count++];
    strncpy(check->name, name, sizeof(check->name) - 1);
    check->check_fn = check_fn;
    check->user_data = user_data;
    check->last_status = HEALTH_UNKNOWN;
    check->last_message[0] = '\0';
    check->last_check_time = 0;
    
    pthread_mutex_unlock(&api_state.mutex);
    
    LOG_HEALTH_DEBUG("Registered health check: %s", name);
    return 0;
}

system_metrics_t* health_api_get_metrics(void)
{
    return &api_state.metrics;
}

void health_api_observe_processing_latency(double latency_ms)
{
    pthread_mutex_lock(&api_state.mutex);
    
    int bucket = find_histogram_bucket(PROCESSING_LATENCY_BUCKETS, NUM_PROCESSING_BUCKETS, latency_ms);
    api_state.metrics.processing_latency_bucket[bucket]++;
    api_state.metrics.processing_latency_count++;
    api_state.metrics.processing_latency_sum += latency_ms;
    
    pthread_mutex_unlock(&api_state.mutex);
}

void health_api_observe_read_latency(double latency_ms)
{
    pthread_mutex_lock(&api_state.mutex);
    
    int bucket = find_histogram_bucket(READ_LATENCY_BUCKETS, NUM_READ_BUCKETS, latency_ms);
    api_state.metrics.read_latency_bucket[bucket]++;
    api_state.metrics.read_latency_count++;
    api_state.metrics.read_latency_sum += latency_ms;
    
    pthread_mutex_unlock(&api_state.mutex);
}

void health_api_inc_counter(uint64_t *counter)
{
    __sync_fetch_and_add(counter, 1);
}

void health_api_add_counter(uint64_t *counter, uint64_t value)
{
    __sync_fetch_and_add(counter, value);
}

void health_api_set_gauge(double *gauge, double value)
{
    pthread_mutex_lock(&api_state.mutex);
    *gauge = value;
    pthread_mutex_unlock(&api_state.mutex);
}
