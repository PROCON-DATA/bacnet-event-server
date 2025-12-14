/**
 * @file redis_cache.c
 * @brief Redis Cache Implementation
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#include "redis_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis/hiredis.h>
#include <cjson/cJSON.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================ */

static struct {
    redisContext *ctx;
    redis_config_t config;
    bool connected;
    char key_buffer[512];
} redis_state = {0};

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static const char* make_key(const char *suffix)
{
    snprintf(redis_state.key_buffer, sizeof(redis_state.key_buffer),
             "%s%s", redis_state.config.key_prefix, suffix);
    return redis_state.key_buffer;
}

static const char* make_object_key(bacnet_object_type_t type, uint32_t instance)
{
    snprintf(redis_state.key_buffer, sizeof(redis_state.key_buffer),
             "%sobject:%d:%u", redis_state.config.key_prefix, type, instance);
    return redis_state.key_buffer;
}

static void log_error(const char *context)
{
    if (redis_state.ctx && redis_state.ctx->err) {
        fprintf(stderr, "[REDIS] %s: %s\n", context, redis_state.ctx->errstr);
    } else {
        fprintf(stderr, "[REDIS] %s: Unknown error\n", context);
    }
}

/* ============================================================================
 * JSON Parsing Helpers
 * ============================================================================ */

static int get_int_safe(const cJSON *json, const char *key, int default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item)) {
        return (int)item->valuedouble;
    }
    return default_val;
}

static unsigned int get_uint_safe(const cJSON *json, const char *key, unsigned int default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item)) {
        return (unsigned int)item->valuedouble;
    }
    return default_val;
}

static double get_double_safe(const cJSON *json, const char *key, double default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return default_val;
}

static bool get_bool_safe(const cJSON *json, const char *key, bool default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

static void get_string_safe(const cJSON *json, const char *key, char *dest, size_t dest_size, const char *default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strncpy(dest, item->valuestring, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else if (default_val) {
        strncpy(dest, default_val, dest_size - 1);
        dest[dest_size - 1] = '\0';
    } else {
        dest[0] = '\0';
    }
}

static long get_long_safe(const cJSON *json, const char *key, long default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item)) {
        return (long)item->valuedouble;
    }
    return default_val;
}

static uint64_t get_uint64_safe(const cJSON *json, const char *key, uint64_t default_val)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item)) {
        return (uint64_t)item->valuedouble;
    }
    return default_val;
}

/* ============================================================================
 * Implementation
 * ============================================================================ */

int redis_cache_init(const redis_config_t *config)
{
    struct timeval timeout;
    
    if (!config || !config->host) {
        fprintf(stderr, "[REDIS] Invalid configuration\n");
        return -1;
    }
    
    /* Store configuration */
    redis_state.config = *config;
    if (!redis_state.config.key_prefix) {
        redis_state.config.key_prefix = "bacnet:";
    }
    
    /* Calculate timeout */
    timeout.tv_sec = config->connection_timeout_ms / 1000;
    timeout.tv_usec = (config->connection_timeout_ms % 1000) * 1000;
    
    /* Establish connection */
    redis_state.ctx = redisConnectWithTimeout(config->host, config->port, timeout);
    
    if (!redis_state.ctx) {
        fprintf(stderr, "[REDIS] Cannot allocate redis context\n");
        return -1;
    }
    
    if (redis_state.ctx->err) {
        log_error("Connection failed");
        redisFree(redis_state.ctx);
        redis_state.ctx = NULL;
        return -1;
    }
    
    /* Authenticate if needed */
    if (config->password && strlen(config->password) > 0) {
        redisReply *reply = redisCommand(redis_state.ctx, "AUTH %s", config->password);
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "[REDIS] Authentication failed\n");
            if (reply) freeReplyObject(reply);
            redisFree(redis_state.ctx);
            redis_state.ctx = NULL;
            return -1;
        }
        freeReplyObject(reply);
    }
    
    /* Select database */
    if (config->database > 0) {
        redisReply *reply = redisCommand(redis_state.ctx, "SELECT %d", config->database);
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            fprintf(stderr, "[REDIS] Database selection failed\n");
            if (reply) freeReplyObject(reply);
            redisFree(redis_state.ctx);
            redis_state.ctx = NULL;
            return -1;
        }
        freeReplyObject(reply);
    }
    
    redis_state.connected = true;
    printf("[REDIS] Connected to %s:%d (db=%d)\n", 
           config->host, config->port, config->database);
    
    return 0;
}

void redis_cache_shutdown(void)
{
    if (redis_state.ctx) {
        redisFree(redis_state.ctx);
        redis_state.ctx = NULL;
    }
    redis_state.connected = false;
    printf("[REDIS] Disconnected\n");
}

bool redis_cache_is_connected(void)
{
    return redis_state.connected && redis_state.ctx && !redis_state.ctx->err;
}

int redis_cache_reconnect(void)
{
    redis_cache_shutdown();
    return redis_cache_init(&redis_state.config);
}

int redis_cache_store_object(const cached_object_t *object)
{
    redisReply *reply;
    const char *key;
    char json[4096];
    int len;
    
    if (!redis_cache_is_connected() || !object) {
        return -1;
    }
    
    key = make_object_key(object->object_type, object->object_instance);
    
    /* Serialize to JSON */
    len = snprintf(json, sizeof(json),
        "{"
        "\"object_type\":%d,"
        "\"object_instance\":%u,"
        "\"object_name\":\"%s\","
        "\"description\":\"%s\","
        "\"value_type\":%d,"
        "\"units\":%u,"
        "\"units_text\":\"%s\","
        "\"cov_increment\":%f,"
        "\"min_value\":%f,"
        "\"max_value\":%f,"
        "\"state_count\":%d,"
        "\"inactive_text\":\"%s\","
        "\"active_text\":\"%s\","
        "\"supports_priority_array\":%s,"
        "\"present_value_real\":%f,"
        "\"present_value_unsigned\":%u,"
        "\"present_value_signed\":%d,"
        "\"present_value_boolean\":%s,"
        "\"status_in_alarm\":%s,"
        "\"status_fault\":%s,"
        "\"status_overridden\":%s,"
        "\"status_out_of_service\":%s,"
        "\"reliability\":%u,"
        "\"event_state\":%u,"
        "\"last_update\":%ld,"
        "\"source_id\":\"%s\","
        "\"stream_position\":%lu"
        "}",
        object->object_type,
        object->object_instance,
        object->object_name,
        object->description,
        object->value_type,
        object->units,
        object->units_text,
        object->cov_increment,
        object->min_value,
        object->max_value,
        object->state_count,
        object->inactive_text,
        object->active_text,
        object->supports_priority_array ? "true" : "false",
        object->present_value.real_value,
        object->present_value.unsigned_value,
        object->present_value.signed_value,
        object->present_value.boolean_value ? "true" : "false",
        object->status_flags.in_alarm ? "true" : "false",
        object->status_flags.fault ? "true" : "false",
        object->status_flags.overridden ? "true" : "false",
        object->status_flags.out_of_service ? "true" : "false",
        object->reliability,
        object->event_state,
        (long)object->last_update,
        object->source_id,
        (unsigned long)object->stream_position
    );
    
    reply = redisCommand(redis_state.ctx, "SET %s %b", key, json, (size_t)len);
    
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        log_error("Store object failed");
        if (reply) freeReplyObject(reply);
        return -1;
    }
    
    freeReplyObject(reply);
    
    /* Add to index */
    reply = redisCommand(redis_state.ctx, "SADD %s %d:%u",
                        make_key("objects:index"),
                        object->object_type,
                        object->object_instance);
    if (reply) freeReplyObject(reply);
    
    return 0;
}

int redis_cache_load_object(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    cached_object_t *object)
{
    redisReply *reply;
    const char *key;
    
    if (!redis_cache_is_connected() || !object) {
        return -1;
    }
    
    key = make_object_key(object_type, object_instance);
    
    reply = redisCommand(redis_state.ctx, "GET %s", key);
    
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        return -1;
    }
    
    /* Parse JSON using cJSON */
    cJSON *json = cJSON_ParseWithLength(reply->str, reply->len);
    if (!json) {
        fprintf(stderr, "[REDIS] Failed to parse JSON for object %d:%u\n", 
                object_type, object_instance);
        freeReplyObject(reply);
        return -1;
    }
    
    /* Initialize object structure */
    memset(object, 0, sizeof(*object));
    
    /* Basic identification */
    object->object_type = (bacnet_object_type_t)get_int_safe(json, "object_type", object_type);
    object->object_instance = get_uint_safe(json, "object_instance", object_instance);
    
    /* Strings */
    get_string_safe(json, "object_name", object->object_name, sizeof(object->object_name), "");
    get_string_safe(json, "description", object->description, sizeof(object->description), "");
    get_string_safe(json, "units_text", object->units_text, sizeof(object->units_text), "");
    get_string_safe(json, "inactive_text", object->inactive_text, sizeof(object->inactive_text), "");
    get_string_safe(json, "active_text", object->active_text, sizeof(object->active_text), "");
    get_string_safe(json, "source_id", object->source_id, sizeof(object->source_id), "");
    
    /* Numeric properties */
    object->value_type = (value_type_t)get_int_safe(json, "value_type", VALUE_TYPE_REAL);
    object->units = get_uint_safe(json, "units", 95); /* 95 = no-units */
    object->cov_increment = (float)get_double_safe(json, "cov_increment", 0.0);
    object->min_value = (float)get_double_safe(json, "min_value", 0.0);
    object->max_value = (float)get_double_safe(json, "max_value", 0.0);
    object->state_count = get_int_safe(json, "state_count", 0);
    object->supports_priority_array = get_bool_safe(json, "supports_priority_array", false);
    
    /* Present value (based on value_type) */
    object->present_value.real_value = (float)get_double_safe(json, "present_value_real", 0.0);
    object->present_value.unsigned_value = get_uint_safe(json, "present_value_unsigned", 0);
    object->present_value.signed_value = get_int_safe(json, "present_value_signed", 0);
    object->present_value.boolean_value = get_bool_safe(json, "present_value_boolean", false);
    
    /* Status flags */
    object->status_flags.in_alarm = get_bool_safe(json, "status_in_alarm", false);
    object->status_flags.fault = get_bool_safe(json, "status_fault", false);
    object->status_flags.overridden = get_bool_safe(json, "status_overridden", false);
    object->status_flags.out_of_service = get_bool_safe(json, "status_out_of_service", false);
    
    /* State */
    object->reliability = (uint8_t)get_uint_safe(json, "reliability", 0);
    object->event_state = (uint8_t)get_uint_safe(json, "event_state", 0);
    object->last_update = (time_t)get_long_safe(json, "last_update", 0);
    object->stream_position = get_uint64_safe(json, "stream_position", 0);
    
    /* Parse state_texts array if present (for multi-state objects) */
    const cJSON *state_texts = cJSON_GetObjectItemCaseSensitive(json, "state_texts");
    if (cJSON_IsArray(state_texts)) {
        int index = 0;
        const cJSON *state_text;
        cJSON_ArrayForEach(state_text, state_texts) {
            if (index >= 16) break;
            if (cJSON_IsString(state_text) && state_text->valuestring) {
                strncpy(object->state_texts[index], state_text->valuestring, 
                        sizeof(object->state_texts[index]) - 1);
                object->state_texts[index][sizeof(object->state_texts[index]) - 1] = '\0';
            }
            index++;
        }
    }
    
    cJSON_Delete(json);
    freeReplyObject(reply);
    return 0;
}

int redis_cache_update_value(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    value_type_t value_type,
    const void *value,
    const status_flags_t *status_flags,
    time_t source_timestamp)
{
    redisReply *reply;
    const char *key;
    
    if (!redis_cache_is_connected() || !value) {
        return -1;
    }
    
    key = make_object_key(object_type, object_instance);
    
    /* HSET for individual fields would be more efficient */
    /* Simplified here with full object update */
    
    cached_object_t obj;
    if (redis_cache_load_object(object_type, object_instance, &obj) != 0) {
        return -1;
    }
    
    /* Update value */
    switch (value_type) {
        case VALUE_TYPE_REAL:
            obj.present_value.real_value = *(const float*)value;
            break;
        case VALUE_TYPE_UNSIGNED:
        case VALUE_TYPE_ENUMERATED:
            obj.present_value.unsigned_value = *(const uint32_t*)value;
            break;
        case VALUE_TYPE_SIGNED:
            obj.present_value.signed_value = *(const int32_t*)value;
            break;
        case VALUE_TYPE_BOOLEAN:
            obj.present_value.boolean_value = *(const bool*)value;
            break;
    }
    
    if (status_flags) {
        obj.status_flags = *status_flags;
    }
    
    obj.last_update = source_timestamp ? source_timestamp : time(NULL);
    
    return redis_cache_store_object(&obj);
}

int redis_cache_delete_object(
    bacnet_object_type_t object_type,
    uint32_t object_instance)
{
    redisReply *reply;
    const char *key;
    
    if (!redis_cache_is_connected()) {
        return -1;
    }
    
    key = make_object_key(object_type, object_instance);
    
    reply = redisCommand(redis_state.ctx, "DEL %s", key);
    if (reply) freeReplyObject(reply);
    
    /* Remove from index */
    reply = redisCommand(redis_state.ctx, "SREM %s %d:%u",
                        make_key("objects:index"),
                        object_type,
                        object_instance);
    if (reply) freeReplyObject(reply);
    
    return 0;
}

int redis_cache_iterate_objects(
    bacnet_object_type_t object_type,
    object_iterator_callback callback,
    void *user_data)
{
    redisReply *reply;
    int count = 0;
    
    if (!redis_cache_is_connected() || !callback) {
        return -1;
    }
    
    /* Get all keys from index */
    reply = redisCommand(redis_state.ctx, "SMEMBERS %s", 
                        make_key("objects:index"));
    
    if (!reply || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) freeReplyObject(reply);
        return -1;
    }
    
    for (size_t i = 0; i < reply->elements; i++) {
        int type;
        uint32_t instance;
        
        if (sscanf(reply->element[i]->str, "%d:%u", &type, &instance) == 2) {
            if (object_type == OBJECT_TYPE_MAX || object_type == type) {
                cached_object_t obj;
                if (redis_cache_load_object(type, instance, &obj) == 0) {
                    callback(&obj, user_data);
                    count++;
                }
            }
        }
    }
    
    freeReplyObject(reply);
    return count;
}

int redis_cache_object_count(bacnet_object_type_t object_type)
{
    redisReply *reply;
    int count = 0;
    
    if (!redis_cache_is_connected()) {
        return -1;
    }
    
    if (object_type == OBJECT_TYPE_MAX) {
        reply = redisCommand(redis_state.ctx, "SCARD %s", 
                            make_key("objects:index"));
        if (reply && reply->type == REDIS_REPLY_INTEGER) {
            count = (int)reply->integer;
        }
        if (reply) freeReplyObject(reply);
    } else {
        /* Would need to iterate and filter */
        reply = redisCommand(redis_state.ctx, "SMEMBERS %s", 
                            make_key("objects:index"));
        if (reply && reply->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < reply->elements; i++) {
                int type;
                uint32_t instance;
                if (sscanf(reply->element[i]->str, "%d:%u", &type, &instance) == 2) {
                    if (type == object_type) count++;
                }
            }
        }
        if (reply) freeReplyObject(reply);
    }
    
    return count;
}

int redis_cache_store_stream_position(const char *subscription_id, uint64_t position)
{
    redisReply *reply;
    
    if (!redis_cache_is_connected() || !subscription_id) {
        return -1;
    }
    
    reply = redisCommand(redis_state.ctx, "HSET %s %s %lu",
                        make_key("stream:positions"),
                        subscription_id,
                        (unsigned long)position);
    
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        log_error("Store stream position failed");
        if (reply) freeReplyObject(reply);
        return -1;
    }
    
    freeReplyObject(reply);
    return 0;
}

int redis_cache_load_stream_position(const char *subscription_id, uint64_t *position)
{
    redisReply *reply;
    
    if (!redis_cache_is_connected() || !subscription_id || !position) {
        return -1;
    }
    
    reply = redisCommand(redis_state.ctx, "HGET %s %s",
                        make_key("stream:positions"),
                        subscription_id);
    
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        if (reply) freeReplyObject(reply);
        return -1;
    }
    
    *position = strtoull(reply->str, NULL, 10);
    freeReplyObject(reply);
    return 0;
}

int redis_cache_store_device_config(
    uint32_t device_instance,
    const char *name,
    const char *description,
    const char *location)
{
    redisReply *reply;
    
    if (!redis_cache_is_connected()) {
        return -1;
    }
    
    reply = redisCommand(redis_state.ctx, 
                        "HMSET %s name %s description %s location %s",
                        make_key("device:config"),
                        name ? name : "",
                        description ? description : "",
                        location ? location : "");
    
    if (!reply || reply->type == REDIS_REPLY_ERROR) {
        log_error("Store device config failed");
        if (reply) freeReplyObject(reply);
        return -1;
    }
    
    freeReplyObject(reply);
    return 0;
}

int redis_cache_publish_value_change(
    bacnet_object_type_t object_type,
    uint32_t object_instance)
{
    redisReply *reply;
    char message[128];
    
    if (!redis_cache_is_connected()) {
        return -1;
    }
    
    snprintf(message, sizeof(message), "%d:%u", object_type, object_instance);
    
    reply = redisCommand(redis_state.ctx, "PUBLISH %s %s",
                        make_key("events:value_change"),
                        message);
    
    if (reply) freeReplyObject(reply);
    return 0;
}
