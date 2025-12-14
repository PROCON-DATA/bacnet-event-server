/**
 * @file main.c
 * @brief BACnet Event Server Main Program
 * 
 * Integrates KurrentDB subscriptions with BACnet server and Redis cache.
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <cjson/cJSON.h>

#include "redis_cache.h"
#include "kurrentdb_client.h"
#include "bacnet_server.h"
#include "message_handler.h"

/* ============================================================================
 * Constants and Global Variables
 * ============================================================================ */

#define CONFIG_FILE_DEFAULT "/etc/bacnet-gateway/server-config.json"
#define MAX_SUBSCRIPTIONS 32

static volatile bool g_running = true;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Subscription handles */
static struct {
    kurrentdb_subscription_handle handle;
    char subscription_id[128];
    uint32_t instance_offset;
    bool active;
} g_subscriptions[MAX_SUBSCRIPTIONS];
static int g_subscription_count = 0;

/* Server configuration */
static struct {
    bacnet_server_config_t bacnet;
    redis_config_t redis;
    kurrentdb_config_t kurrentdb;
} g_config;

/* ============================================================================
 * Signal Handler
 * ============================================================================ */

static void signal_handler(int sig)
{
    printf("\n[MAIN] Signal %d received, shutting down...\n", sig);
    g_running = false;
}

/* ============================================================================
 * Load Configuration
 * ============================================================================ */

static int load_config(const char *filename)
{
    FILE *fp;
    long file_size;
    char *json_str;
    cJSON *root = NULL;
    cJSON *item, *sub_item;
    int result = -1;
    
    fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "[CONFIG] Cannot open config file: %s\n", filename);
        return -1;
    }
    
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    json_str = (char*)malloc(file_size + 1);
    if (!json_str) {
        fclose(fp);
        return -1;
    }
    
    fread(json_str, 1, file_size, fp);
    json_str[file_size] = '\0';
    fclose(fp);
    
    root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root) {
        fprintf(stderr, "[CONFIG] JSON parse error\n");
        return -1;
    }
    
    /* Server configuration */
    item = cJSON_GetObjectItemCaseSensitive(root, "server");
    if (item) {
        g_config.bacnet.device_instance = (uint32_t)cJSON_GetObjectItemCaseSensitive(item, "deviceInstance")->valuedouble;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "deviceName");
        if (sub_item) g_config.bacnet.device_name = strdup(sub_item->valuestring);
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "deviceDescription");
        if (sub_item) g_config.bacnet.device_description = strdup(sub_item->valuestring);
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "vendorId");
        if (sub_item) g_config.bacnet.vendor_id = (uint16_t)sub_item->valuedouble;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "vendorName");
        if (sub_item) g_config.bacnet.vendor_name = strdup(sub_item->valuestring);
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "modelName");
        if (sub_item) g_config.bacnet.model_name = strdup(sub_item->valuestring);
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "port");
        g_config.bacnet.port = sub_item ? (int)sub_item->valuedouble : 47808;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "interface");
        if (sub_item) g_config.bacnet.interface = strdup(sub_item->valuestring);
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "covLifetime");
        g_config.bacnet.cov_lifetime = sub_item ? (uint32_t)sub_item->valuedouble : 300;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "maxCovSubscriptions");
        g_config.bacnet.max_cov_subscriptions = sub_item ? (int)sub_item->valuedouble : 100;
    }
    
    /* Redis configuration */
    item = cJSON_GetObjectItemCaseSensitive(root, "redis");
    if (item) {
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "host");
        g_config.redis.host = sub_item ? strdup(sub_item->valuestring) : "localhost";
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "port");
        g_config.redis.port = sub_item ? (int)sub_item->valuedouble : 6379;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "password");
        g_config.redis.password = sub_item ? strdup(sub_item->valuestring) : NULL;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "database");
        g_config.redis.database = sub_item ? (int)sub_item->valuedouble : 0;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "keyPrefix");
        g_config.redis.key_prefix = sub_item ? strdup(sub_item->valuestring) : "bacnet:";
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "connectionTimeout");
        g_config.redis.connection_timeout_ms = sub_item ? (int)sub_item->valuedouble : 5000;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "commandTimeout");
        g_config.redis.command_timeout_ms = sub_item ? (int)sub_item->valuedouble : 1000;
    }
    
    /* KurrentDB configuration */
    item = cJSON_GetObjectItemCaseSensitive(root, "kurrentdb");
    if (item) {
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "connectionString");
        if (sub_item) g_config.kurrentdb.connection_string = strdup(sub_item->valuestring);
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "tlsEnabled");
        g_config.kurrentdb.tls_enabled = sub_item ? cJSON_IsTrue(sub_item) : true;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "tlsVerifyCert");
        g_config.kurrentdb.tls_verify_cert = sub_item ? cJSON_IsTrue(sub_item) : true;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "reconnectDelayMs");
        g_config.kurrentdb.reconnect_delay_ms = sub_item ? (int)sub_item->valuedouble : 5000;
        
        sub_item = cJSON_GetObjectItemCaseSensitive(item, "maxReconnectAttempts");
        g_config.kurrentdb.max_reconnect_attempts = sub_item ? (int)sub_item->valuedouble : -1;
    }
    
    result = 0;
    
    if (root) cJSON_Delete(root);
    return result;
}

static int load_subscriptions(const char *filename)
{
    FILE *fp;
    long file_size;
    char *json_str;
    cJSON *root = NULL;
    cJSON *devices, *device;
    int count = 0;
    
    fp = fopen(filename, "r");
    if (!fp) return -1;
    
    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    json_str = (char*)malloc(file_size + 1);
    if (!json_str) {
        fclose(fp);
        return -1;
    }
    
    fread(json_str, 1, file_size, fp);
    json_str[file_size] = '\0';
    fclose(fp);
    
    root = cJSON_Parse(json_str);
    free(json_str);
    
    if (!root) return -1;
    
    devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(devices)) {
        cJSON_Delete(root);
        return -1;
    }
    
    cJSON_ArrayForEach(device, devices) {
        if (count >= MAX_SUBSCRIPTIONS) break;
        
        cJSON *sub_id = cJSON_GetObjectItemCaseSensitive(device, "subscriptionId");
        cJSON *stream = cJSON_GetObjectItemCaseSensitive(device, "streamName");
        cJSON *offset = cJSON_GetObjectItemCaseSensitive(device, "objectInstanceOffset");
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(device, "enabled");
        
        if (!sub_id || !stream) continue;
        if (enabled && !cJSON_IsTrue(enabled)) continue;
        
        strncpy(g_subscriptions[count].subscription_id, 
                sub_id->valuestring, 
                sizeof(g_subscriptions[count].subscription_id) - 1);
        g_subscriptions[count].instance_offset = offset ? (uint32_t)offset->valuedouble : 0;
        g_subscriptions[count].active = true;
        
        count++;
    }
    
    g_subscription_count = count;
    
    cJSON_Delete(root);
    return count;
}

/* ============================================================================
 * Event Callbacks
 * ============================================================================ */

static bool on_event_received(const received_event_t *event, void *user_data)
{
    parsed_message_t message;
    parse_result_t parse_result;
    uint32_t instance_offset = 0;
    
    if (!event || !event->data) {
        return true;  /* ACK anyway */
    }
    
    /* Find instance offset */
    for (int i = 0; i < g_subscription_count; i++) {
        if (strcmp(g_subscriptions[i].subscription_id, event->subscription_id) == 0) {
            instance_offset = g_subscriptions[i].instance_offset;
            break;
        }
    }
    
    /* Parse message */
    parse_result = message_handler_parse(event->data, event->data_length, &message);
    
    if (parse_result != PARSE_OK) {
        fprintf(stderr, "[EVENT] Parse error: %s\n", 
                message_handler_error_string(parse_result));
        return true;  /* ACK - don't retry */
    }
    
    /* Process message */
    pthread_mutex_lock(&g_mutex);
    
    int result = message_handler_process(&message, event->subscription_id, instance_offset);
    
    /* Store stream position */
    if (result == 0) {
        redis_cache_store_stream_position(
            event->subscription_id, 
            event->metadata.stream_revision
        );
    }
    
    pthread_mutex_unlock(&g_mutex);
    
    return (result == 0);
}

static void on_subscription_error(
    const char *subscription_id,
    const char *error_message,
    void *user_data)
{
    fprintf(stderr, "[SUBSCRIPTION] Error in %s: %s\n", subscription_id, error_message);
}

static void on_connection_status(bool connected, void *user_data)
{
    if (connected) {
        printf("[KURRENTDB] Connected\n");
    } else {
        printf("[KURRENTDB] Disconnected, attempting reconnect...\n");
    }
}

/* ============================================================================
 * Cache Recovery
 * ============================================================================ */

static void load_object_callback(const cached_object_t *object, void *user_data)
{
    int *count = (int*)user_data;
    
    if (bacnet_server_create_object(object) == 0) {
        (*count)++;
    }
}

static int recover_from_cache(void)
{
    int count = 0;
    
    printf("[RECOVERY] Loading objects from Redis cache...\n");
    
    redis_cache_iterate_objects(OBJECT_TYPE_MAX, load_object_callback, &count);
    
    printf("[RECOVERY] Loaded %d objects from cache\n", count);
    
    return count;
}

/* ============================================================================
 * Main Loop
 * ============================================================================ */

static void* bacnet_task_thread(void *arg)
{
    time_t last_cov_update = time(NULL);
    
    while (g_running) {
        pthread_mutex_lock(&g_mutex);
        
        /* Process BACnet messages */
        bacnet_server_task(100);
        
        /* Update COV lifetimes (every second) */
        time_t now = time(NULL);
        if (now > last_cov_update) {
            bacnet_cov_update_lifetimes((int)(now - last_cov_update));
            last_cov_update = now;
        }
        
        pthread_mutex_unlock(&g_mutex);
        
        usleep(10000);  /* 10ms */
    }
    
    return NULL;
}

/* ============================================================================
 * Main Program
 * ============================================================================ */

int main(int argc, char *argv[])
{
    const char *config_file = CONFIG_FILE_DEFAULT;
    pthread_t bacnet_thread;
    int i;
    
    printf("========================================\n");
    printf(" BACnet Event Server\n");
    printf(" Unlock Europe - Free and Open Source Software - Energy\n");
    printf("========================================\n\n");
    
    /* Command line arguments */
    if (argc > 1) {
        config_file = argv[1];
    }
    
    /* Signal handler */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Load configuration */
    printf("[MAIN] Loading configuration from %s\n", config_file);
    if (load_config(config_file) != 0) {
        fprintf(stderr, "[MAIN] Failed to load configuration\n");
        return 1;
    }
    
    /* Load subscriptions */
    if (load_subscriptions(config_file) <= 0) {
        fprintf(stderr, "[MAIN] No subscriptions configured\n");
        return 1;
    }
    printf("[MAIN] Loaded %d subscription(s)\n", g_subscription_count);
    
    /* Initialize message handler */
    if (message_handler_init() != 0) {
        fprintf(stderr, "[MAIN] Failed to initialize message handler\n");
        return 1;
    }
    
    /* Initialize Redis cache */
    printf("[MAIN] Connecting to Redis at %s:%d\n", 
           g_config.redis.host, g_config.redis.port);
    if (redis_cache_init(&g_config.redis) != 0) {
        fprintf(stderr, "[MAIN] Failed to connect to Redis\n");
        return 1;
    }
    
    /* Initialize BACnet server */
    printf("[MAIN] Starting BACnet server (Device Instance: %u)\n", 
           g_config.bacnet.device_instance);
    if (bacnet_server_init(&g_config.bacnet) != 0) {
        fprintf(stderr, "[MAIN] Failed to initialize BACnet server\n");
        redis_cache_shutdown();
        return 1;
    }
    
    /* Restore objects from cache */
    recover_from_cache();
    
    /* Start BACnet server */
    if (bacnet_server_start() != 0) {
        fprintf(stderr, "[MAIN] Failed to start BACnet server\n");
        bacnet_server_shutdown();
        redis_cache_shutdown();
        return 1;
    }
    
    /* Initialize KurrentDB */
    printf("[MAIN] Connecting to KurrentDB...\n");
    if (kurrentdb_init(&g_config.kurrentdb) != 0) {
        fprintf(stderr, "[MAIN] Failed to connect to KurrentDB\n");
        bacnet_server_shutdown();
        redis_cache_shutdown();
        return 1;
    }
    
    kurrentdb_set_connection_callback(on_connection_status, NULL);
    
    /* Start subscriptions */
    for (i = 0; i < g_subscription_count; i++) {
        subscription_config_t sub_config = {0};
        uint64_t last_position = 0;
        
        /* Load last position from cache */
        if (redis_cache_load_stream_position(
                g_subscriptions[i].subscription_id, 
                &last_position) == 0) {
            printf("[MAIN] Resuming subscription %s from position %lu\n",
                   g_subscriptions[i].subscription_id, 
                   (unsigned long)last_position);
            sub_config.start_from = SUBSCRIPTION_START_POSITION;
            sub_config.start_position = last_position + 1;
        } else {
            sub_config.start_from = SUBSCRIPTION_START_BEGIN;
        }
        
        sub_config.subscription_id = g_subscriptions[i].subscription_id;
        sub_config.object_instance_offset = g_subscriptions[i].instance_offset;
        sub_config.enabled = true;
        
        g_subscriptions[i].handle = kurrentdb_subscribe_persistent(
            &sub_config,
            on_event_received,
            on_subscription_error,
            NULL
        );
        
        if (!g_subscriptions[i].handle) {
            fprintf(stderr, "[MAIN] Failed to subscribe to %s\n",
                    g_subscriptions[i].subscription_id);
        } else {
            printf("[MAIN] Subscribed to %s (offset=%u)\n",
                   g_subscriptions[i].subscription_id,
                   g_subscriptions[i].instance_offset);
        }
    }
    
    /* Start BACnet task thread */
    if (pthread_create(&bacnet_thread, NULL, bacnet_task_thread, NULL) != 0) {
        fprintf(stderr, "[MAIN] Failed to create BACnet thread\n");
    }
    
    /* Start KurrentDB event loop */
    printf("[MAIN] Server running. Press Ctrl+C to stop.\n\n");
    kurrentdb_start_event_loop();
    
    /* Main loop */
    while (g_running) {
        sleep(1);
        
        /* Output statistics */
        if (!g_running) break;
        
        server_stats_t stats;
        bacnet_server_get_stats(&stats);
        
        /* Output status every 60 seconds */
        static int status_counter = 0;
        if (++status_counter >= 60) {
            printf("[STATUS] Objects: %lu, COV Subscriptions: %lu, "
                   "Read Requests: %lu, COV Notifications: %lu\n",
                   (unsigned long)stats.objects_total,
                   (unsigned long)stats.cov_subscriptions_active,
                   (unsigned long)stats.read_requests,
                   (unsigned long)stats.cov_notifications_sent);
            status_counter = 0;
        }
    }
    
    /* Cleanup */
    printf("\n[MAIN] Shutting down...\n");
    
    kurrentdb_stop_event_loop();
    
    for (i = 0; i < g_subscription_count; i++) {
        if (g_subscriptions[i].handle) {
            kurrentdb_unsubscribe(g_subscriptions[i].handle);
        }
    }
    
    pthread_join(bacnet_thread, NULL);
    
    kurrentdb_shutdown();
    bacnet_server_shutdown();
    redis_cache_shutdown();
    message_handler_shutdown();
    
    printf("[MAIN] Goodbye!\n");
    
    return 0;
}
