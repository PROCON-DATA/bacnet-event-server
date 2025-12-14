/**
 * @file message_handler.c
 * @brief JSON Message Handler Implementation
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#include "message_handler.h"
#include "redis_cache.h"
#include "bacnet_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <cjson/cJSON.h>

/* ============================================================================
 * ISO 8601 Timestamp Parser
 * ============================================================================ */

/**
 * @brief Parse ISO 8601 timestamp to milliseconds since epoch
 * 
 * Supports formats:
 * - 2024-12-14T10:30:00Z
 * - 2024-12-14T10:30:00.123Z
 * - 2024-12-14T10:30:00+01:00
 * - 2024-12-14T10:30:00.123+01:00
 * 
 * @param iso_str ISO 8601 timestamp string
 * @param out_ms Output: milliseconds since Unix epoch
 * @return 0 on success, -1 on error
 */
static int parse_iso8601_timestamp(const char *iso_str, int64_t *out_ms)
{
    struct tm tm_time = {0};
    int year, month, day, hour, minute, second;
    int milliseconds = 0;
    int tz_offset_minutes = 0;
    const char *ptr;
    
    if (!iso_str || !out_ms) {
        return -1;
    }
    
    /* Parse date: YYYY-MM-DD */
    if (sscanf(iso_str, "%d-%d-%d", &year, &month, &day) != 3) {
        return -1;
    }
    
    /* Find 'T' separator */
    ptr = strchr(iso_str, 'T');
    if (!ptr) {
        ptr = strchr(iso_str, ' '); /* Alternative: space separator */
    }
    if (!ptr) {
        return -1;
    }
    ptr++; /* Skip separator */
    
    /* Parse time: HH:MM:SS */
    if (sscanf(ptr, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return -1;
    }
    
    /* Look for milliseconds (.xxx) */
    ptr = strchr(ptr, '.');
    if (ptr) {
        ptr++; /* Skip '.' */
        char ms_str[4] = "000";
        int i = 0;
        while (i < 3 && isdigit((unsigned char)*ptr)) {
            ms_str[i++] = *ptr++;
        }
        milliseconds = atoi(ms_str);
    }
    
    /* Look for timezone: Z, +HH:MM, -HH:MM */
    ptr = iso_str;
    while (*ptr && *ptr != 'Z' && *ptr != '+' && (*ptr != '-' || ptr == iso_str || *(ptr-1) != ':')) {
        /* Find timezone indicator, but don't match the '-' in the date */
        if (*ptr == '-' && ptr > iso_str + 10) {
            break; /* This '-' is after date portion, could be timezone */
        }
        ptr++;
    }
    
    if (*ptr == 'Z') {
        tz_offset_minutes = 0; /* UTC */
    } else if (*ptr == '+' || *ptr == '-') {
        int tz_hour = 0, tz_min = 0;
        char sign = *ptr;
        ptr++;
        if (sscanf(ptr, "%d:%d", &tz_hour, &tz_min) >= 1) {
            tz_offset_minutes = tz_hour * 60 + tz_min;
            if (sign == '-') {
                tz_offset_minutes = -tz_offset_minutes;
            }
        }
    }
    
    /* Build struct tm */
    tm_time.tm_year = year - 1900;
    tm_time.tm_mon = month - 1;
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = minute;
    tm_time.tm_sec = second;
    tm_time.tm_isdst = 0;
    
    /* Convert to time_t (assuming UTC input, then adjust for timezone) */
#ifdef _WIN32
    time_t epoch_sec = _mkgmtime(&tm_time);
#else
    time_t epoch_sec = timegm(&tm_time);
#endif
    
    if (epoch_sec == (time_t)-1) {
        return -1;
    }
    
    /* Adjust for timezone offset (convert to UTC) */
    epoch_sec -= (tz_offset_minutes * 60);
    
    /* Convert to milliseconds */
    *out_ms = (int64_t)epoch_sec * 1000 + milliseconds;
    
    return 0;
}

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static const char* get_string_safe(cJSON *obj, const char *key, const char *default_val)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return default_val;
}

static double get_number_safe(cJSON *obj, const char *key, double default_val)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return default_val;
}

static bool get_bool_safe(cJSON *obj, const char *key, bool default_val)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item);
    }
    return default_val;
}

static message_type_t parse_message_type(const char *type_str)
{
    if (!type_str) return MSG_TYPE_UNKNOWN;
    
    if (strcmp(type_str, "ObjectDefinition") == 0) return MSG_TYPE_OBJECT_DEFINITION;
    if (strcmp(type_str, "ValueUpdate") == 0) return MSG_TYPE_VALUE_UPDATE;
    if (strcmp(type_str, "ObjectDelete") == 0) return MSG_TYPE_OBJECT_DELETE;
    if (strcmp(type_str, "DeviceConfig") == 0) return MSG_TYPE_DEVICE_CONFIG;
    
    return MSG_TYPE_UNKNOWN;
}

static bacnet_object_type_t parse_object_type(const char *type_str)
{
    if (!type_str) return -1;
    
    if (strcmp(type_str, "analog-input") == 0) return OBJECT_ANALOG_INPUT;
    if (strcmp(type_str, "analog-output") == 0) return OBJECT_ANALOG_OUTPUT;
    if (strcmp(type_str, "analog-value") == 0) return OBJECT_ANALOG_VALUE;
    if (strcmp(type_str, "binary-input") == 0) return OBJECT_BINARY_INPUT;
    if (strcmp(type_str, "binary-output") == 0) return OBJECT_BINARY_OUTPUT;
    if (strcmp(type_str, "binary-value") == 0) return OBJECT_BINARY_VALUE;
    if (strcmp(type_str, "multi-state-input") == 0) return OBJECT_MULTI_STATE_INPUT;
    if (strcmp(type_str, "multi-state-output") == 0) return OBJECT_MULTI_STATE_OUTPUT;
    if (strcmp(type_str, "multi-state-value") == 0) return OBJECT_MULTI_STATE_VALUE;
    
    return -1;
}

static value_type_t parse_value_type(const char *type_str)
{
    if (!type_str) return VALUE_TYPE_REAL;
    
    if (strcmp(type_str, "real") == 0) return VALUE_TYPE_REAL;
    if (strcmp(type_str, "unsigned") == 0) return VALUE_TYPE_UNSIGNED;
    if (strcmp(type_str, "signed") == 0) return VALUE_TYPE_SIGNED;
    if (strcmp(type_str, "boolean") == 0) return VALUE_TYPE_BOOLEAN;
    if (strcmp(type_str, "enumerated") == 0) return VALUE_TYPE_ENUMERATED;
    
    return VALUE_TYPE_REAL;
}

/* ============================================================================
 * Parse Functions
 * ============================================================================ */

static parse_result_t parse_object_definition(cJSON *payload, object_definition_msg_t *msg)
{
    const char *str;
    cJSON *item;
    
    /* Required fields */
    str = get_string_safe(payload, "objectType", NULL);
    if (!str) return PARSE_ERROR_MISSING_FIELD;
    msg->object_type = parse_object_type(str);
    if ((int)msg->object_type < 0) return PARSE_ERROR_INVALID_VALUE;
    
    item = cJSON_GetObjectItemCaseSensitive(payload, "objectInstance");
    if (!cJSON_IsNumber(item)) return PARSE_ERROR_MISSING_FIELD;
    msg->object_instance = (uint32_t)item->valuedouble;
    
    str = get_string_safe(payload, "objectName", NULL);
    if (!str) return PARSE_ERROR_MISSING_FIELD;
    strncpy(msg->object_name, str, sizeof(msg->object_name) - 1);
    
    str = get_string_safe(payload, "presentValueType", NULL);
    if (!str) return PARSE_ERROR_MISSING_FIELD;
    msg->value_type = parse_value_type(str);
    
    /* Optional fields */
    str = get_string_safe(payload, "description", "");
    strncpy(msg->description, str, sizeof(msg->description) - 1);
    
    msg->units = (uint32_t)get_number_safe(payload, "units", 95); /* 95 = no-units */
    
    str = get_string_safe(payload, "unitsText", "");
    strncpy(msg->units_text, str, sizeof(msg->units_text) - 1);
    
    msg->cov_increment = (float)get_number_safe(payload, "covIncrement", 0.0);
    msg->min_value = (float)get_number_safe(payload, "minPresentValue", 0.0);
    msg->max_value = (float)get_number_safe(payload, "maxPresentValue", 0.0);
    
    /* State texts for multi-state */
    item = cJSON_GetObjectItemCaseSensitive(payload, "stateTexts");
    if (cJSON_IsArray(item)) {
        msg->state_count = cJSON_GetArraySize(item);
        if (msg->state_count > 16) msg->state_count = 16;
        for (int i = 0; i < msg->state_count; i++) {
            cJSON *state = cJSON_GetArrayItem(item, i);
            if (cJSON_IsString(state)) {
                strncpy(msg->state_texts[i], state->valuestring, 63);
            }
        }
    }
    
    /* Binary texts */
    str = get_string_safe(payload, "inactiveText", "Inactive");
    strncpy(msg->inactive_text, str, sizeof(msg->inactive_text) - 1);
    
    str = get_string_safe(payload, "activeText", "Active");
    strncpy(msg->active_text, str, sizeof(msg->active_text) - 1);
    
    msg->supports_priority_array = get_bool_safe(payload, "priorityArray", false);
    
    /* Initial value */
    item = cJSON_GetObjectItemCaseSensitive(payload, "initialValue");
    if (item) {
        msg->has_initial_value = true;
        if (cJSON_IsNumber(item)) {
            if (msg->value_type == VALUE_TYPE_REAL) {
                msg->initial_value.real_value = (float)item->valuedouble;
            } else if (msg->value_type == VALUE_TYPE_SIGNED) {
                msg->initial_value.signed_value = (int32_t)item->valuedouble;
            } else {
                msg->initial_value.unsigned_value = (uint32_t)item->valuedouble;
            }
        } else if (cJSON_IsBool(item)) {
            msg->initial_value.boolean_value = cJSON_IsTrue(item);
        }
    }
    
    return PARSE_OK;
}

static parse_result_t parse_value_update(cJSON *payload, value_update_msg_t *msg)
{
    const char *str;
    cJSON *item;
    
    /* Required fields */
    str = get_string_safe(payload, "objectType", NULL);
    if (!str) return PARSE_ERROR_MISSING_FIELD;
    msg->object_type = parse_object_type(str);
    if ((int)msg->object_type < 0) return PARSE_ERROR_INVALID_VALUE;
    
    item = cJSON_GetObjectItemCaseSensitive(payload, "objectInstance");
    if (!cJSON_IsNumber(item)) return PARSE_ERROR_MISSING_FIELD;
    msg->object_instance = (uint32_t)item->valuedouble;
    
    item = cJSON_GetObjectItemCaseSensitive(payload, "presentValue");
    if (!item) return PARSE_ERROR_MISSING_FIELD;
    
    /* Derive value type from object type if not explicit */
    switch (msg->object_type) {
        case OBJECT_BINARY_INPUT:
        case OBJECT_BINARY_OUTPUT:
        case OBJECT_BINARY_VALUE:
            msg->value_type = VALUE_TYPE_BOOLEAN;
            msg->present_value.boolean_value = cJSON_IsTrue(item) || 
                (cJSON_IsNumber(item) && item->valuedouble != 0);
            break;
            
        case OBJECT_MULTI_STATE_INPUT:
        case OBJECT_MULTI_STATE_OUTPUT:
        case OBJECT_MULTI_STATE_VALUE:
            msg->value_type = VALUE_TYPE_UNSIGNED;
            msg->present_value.unsigned_value = (uint32_t)item->valuedouble;
            break;
            
        default:
            msg->value_type = VALUE_TYPE_REAL;
            msg->present_value.real_value = (float)item->valuedouble;
            break;
    }
    
    /* Optional fields */
    str = get_string_safe(payload, "quality", "good");
    strncpy(msg->quality, str, sizeof(msg->quality) - 1);
    
    /* Status flags */
    item = cJSON_GetObjectItemCaseSensitive(payload, "statusFlags");
    if (cJSON_IsObject(item)) {
        msg->has_status_flags = true;
        msg->status_flags.in_alarm = get_bool_safe(item, "inAlarm", false);
        msg->status_flags.fault = get_bool_safe(item, "fault", false);
        msg->status_flags.overridden = get_bool_safe(item, "overridden", false);
        msg->status_flags.out_of_service = get_bool_safe(item, "outOfService", false);
    }
    
    /* Priority */
    item = cJSON_GetObjectItemCaseSensitive(payload, "priority");
    if (cJSON_IsNumber(item)) {
        msg->has_priority = true;
        msg->priority = (uint8_t)item->valuedouble;
    }
    
    /* Source timestamp */
    str = get_string_safe(payload, "sourceTimestamp", NULL);
    if (str) {
        int64_t timestamp_ms;
        if (parse_iso8601_timestamp(str, &timestamp_ms) == 0) {
            msg->has_source_timestamp = true;
            msg->source_timestamp_ms = timestamp_ms;
        } else {
            fprintf(stderr, "[MSG] Failed to parse sourceTimestamp: %s\n", str);
            msg->has_source_timestamp = false;
        }
    }
    
    return PARSE_OK;
}

static parse_result_t parse_object_delete(cJSON *payload, object_delete_msg_t *msg)
{
    const char *str;
    cJSON *item;
    
    str = get_string_safe(payload, "objectType", NULL);
    if (!str) return PARSE_ERROR_MISSING_FIELD;
    msg->object_type = parse_object_type(str);
    if ((int)msg->object_type < 0) return PARSE_ERROR_INVALID_VALUE;
    
    item = cJSON_GetObjectItemCaseSensitive(payload, "objectInstance");
    if (!cJSON_IsNumber(item)) return PARSE_ERROR_MISSING_FIELD;
    msg->object_instance = (uint32_t)item->valuedouble;
    
    str = get_string_safe(payload, "reason", "");
    strncpy(msg->reason, str, sizeof(msg->reason) - 1);
    
    return PARSE_OK;
}

static parse_result_t parse_device_config(cJSON *payload, device_config_msg_t *msg)
{
    const char *str;
    
    memset(msg, 0, sizeof(*msg));
    
    str = get_string_safe(payload, "deviceName", NULL);
    if (str) {
        msg->has_device_name = true;
        strncpy(msg->device_name, str, sizeof(msg->device_name) - 1);
    }
    
    str = get_string_safe(payload, "deviceDescription", NULL);
    if (str) {
        msg->has_device_description = true;
        strncpy(msg->device_description, str, sizeof(msg->device_description) - 1);
    }
    
    str = get_string_safe(payload, "location", NULL);
    if (str) {
        msg->has_location = true;
        strncpy(msg->location, str, sizeof(msg->location) - 1);
    }
    
    str = get_string_safe(payload, "modelName", NULL);
    if (str) {
        msg->has_model_name = true;
        strncpy(msg->model_name, str, sizeof(msg->model_name) - 1);
    }
    
    str = get_string_safe(payload, "vendorName", NULL);
    if (str) {
        msg->has_vendor_name = true;
        strncpy(msg->vendor_name, str, sizeof(msg->vendor_name) - 1);
    }
    
    str = get_string_safe(payload, "applicationSoftwareVersion", NULL);
    if (str) {
        msg->has_application_version = true;
        strncpy(msg->application_version, str, sizeof(msg->application_version) - 1);
    }
    
    return PARSE_OK;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int message_handler_init(void)
{
    /* Nothing to initialize currently */
    return 0;
}

void message_handler_shutdown(void)
{
    /* Nothing to clean up currently */
}

parse_result_t message_handler_parse(
    const char *json,
    size_t json_length,
    parsed_message_t *message)
{
    cJSON *root = NULL;
    cJSON *payload = NULL;
    parse_result_t result = PARSE_OK;
    const char *str;
    
    if (!json || !message) {
        return PARSE_ERROR_INVALID_JSON;
    }
    
    memset(message, 0, sizeof(*message));
    
    root = cJSON_ParseWithLength(json, json_length);
    if (!root) {
        return PARSE_ERROR_INVALID_JSON;
    }
    
    /* Message type */
    str = get_string_safe(root, "messageType", NULL);
    if (!str) {
        result = PARSE_ERROR_MISSING_FIELD;
        goto cleanup;
    }
    message->type = parse_message_type(str);
    if (message->type == MSG_TYPE_UNKNOWN) {
        result = PARSE_ERROR_UNKNOWN_MESSAGE_TYPE;
        goto cleanup;
    }
    
    /* Source ID */
    str = get_string_safe(root, "sourceId", NULL);
    if (!str) {
        result = PARSE_ERROR_MISSING_FIELD;
        goto cleanup;
    }
    strncpy(message->source_id, str, sizeof(message->source_id) - 1);
    
    /* Timestamp (optional parsing) */
    str = get_string_safe(root, "timestamp", NULL);
    if (str) {
        /* ISO 8601 parsing would happen here */
        message->timestamp_ms = time(NULL) * 1000;
    }
    
    /* Stream position */
    message->stream_position = (uint64_t)get_number_safe(root, "streamPosition", 0);
    
    /* Correlation ID */
    str = get_string_safe(root, "correlationId", "");
    strncpy(message->correlation_id, str, sizeof(message->correlation_id) - 1);
    
    /* Payload */
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload)) {
        result = PARSE_ERROR_MISSING_FIELD;
        goto cleanup;
    }
    
    switch (message->type) {
        case MSG_TYPE_OBJECT_DEFINITION:
            result = parse_object_definition(payload, &message->payload.object_definition);
            break;
            
        case MSG_TYPE_VALUE_UPDATE:
            result = parse_value_update(payload, &message->payload.value_update);
            break;
            
        case MSG_TYPE_OBJECT_DELETE:
            result = parse_object_delete(payload, &message->payload.object_delete);
            break;
            
        case MSG_TYPE_DEVICE_CONFIG:
            result = parse_device_config(payload, &message->payload.device_config);
            break;
            
        default:
            result = PARSE_ERROR_UNKNOWN_MESSAGE_TYPE;
            break;
    }
    
cleanup:
    if (root) cJSON_Delete(root);
    return result;
}

int message_handler_process(
    const parsed_message_t *message,
    const char *subscription_id,
    uint32_t instance_offset)
{
    if (!message) return -1;
    
    switch (message->type) {
        case MSG_TYPE_OBJECT_DEFINITION:
            return message_handler_process_object_definition(
                &message->payload.object_definition, instance_offset);
            
        case MSG_TYPE_VALUE_UPDATE:
            return message_handler_process_value_update(
                &message->payload.value_update, instance_offset, message->source_id);
            
        case MSG_TYPE_OBJECT_DELETE:
            return message_handler_process_object_delete(
                &message->payload.object_delete, instance_offset);
            
        case MSG_TYPE_DEVICE_CONFIG:
            return message_handler_process_device_config(
                &message->payload.device_config);
            
        default:
            return -1;
    }
}

int message_handler_process_object_definition(
    const object_definition_msg_t *msg,
    uint32_t instance_offset)
{
    cached_object_t obj;
    
    if (!msg) return -1;
    
    memset(&obj, 0, sizeof(obj));
    
    obj.object_type = msg->object_type;
    obj.object_instance = msg->object_instance + instance_offset;
    strncpy(obj.object_name, msg->object_name, sizeof(obj.object_name) - 1);
    strncpy(obj.description, msg->description, sizeof(obj.description) - 1);
    obj.value_type = msg->value_type;
    obj.units = msg->units;
    strncpy(obj.units_text, msg->units_text, sizeof(obj.units_text) - 1);
    obj.cov_increment = msg->cov_increment;
    obj.min_value = msg->min_value;
    obj.max_value = msg->max_value;
    obj.state_count = msg->state_count;
    for (int i = 0; i < msg->state_count; i++) {
        strncpy(obj.state_texts[i], msg->state_texts[i], 63);
    }
    strncpy(obj.inactive_text, msg->inactive_text, sizeof(obj.inactive_text) - 1);
    strncpy(obj.active_text, msg->active_text, sizeof(obj.active_text) - 1);
    obj.supports_priority_array = msg->supports_priority_array;
    
    if (msg->has_initial_value) {
        obj.present_value = msg->initial_value;
    }
    
    obj.last_update = time(NULL);
    
    /* Store in cache */
    if (redis_cache_store_object(&obj) != 0) {
        fprintf(stderr, "[MSG] Failed to store object in cache\n");
        return -1;
    }
    
    /* Create BACnet object */
    if (bacnet_server_create_object(&obj) != 0) {
        fprintf(stderr, "[MSG] Failed to create BACnet object\n");
        return -1;
    }
    
    printf("[MSG] Created object %s (type=%d, instance=%u)\n",
           obj.object_name, obj.object_type, obj.object_instance);
    
    return 0;
}

int message_handler_process_value_update(
    const value_update_msg_t *msg,
    uint32_t instance_offset,
    const char *source_id)
{
    uint32_t instance;
    const void *value_ptr;
    
    if (!msg) return -1;
    
    instance = msg->object_instance + instance_offset;
    
    /* Determine value pointer */
    switch (msg->value_type) {
        case VALUE_TYPE_REAL:
            value_ptr = &msg->present_value.real_value;
            break;
        case VALUE_TYPE_UNSIGNED:
        case VALUE_TYPE_ENUMERATED:
            value_ptr = &msg->present_value.unsigned_value;
            break;
        case VALUE_TYPE_SIGNED:
            value_ptr = &msg->present_value.signed_value;
            break;
        case VALUE_TYPE_BOOLEAN:
            value_ptr = &msg->present_value.boolean_value;
            break;
        default:
            return -1;
    }
    
    /* Update cache */
    if (redis_cache_update_value(
            msg->object_type,
            instance,
            msg->value_type,
            value_ptr,
            msg->has_status_flags ? &msg->status_flags : NULL,
            time(NULL)) != 0) {
        fprintf(stderr, "[MSG] Failed to update cache\n");
        return -1;
    }
    
    /* Update BACnet object (triggers COV) */
    if (bacnet_server_update_value(
            msg->object_type,
            instance,
            msg->value_type,
            value_ptr,
            msg->has_status_flags ? &msg->status_flags : NULL) != 0) {
        fprintf(stderr, "[MSG] Failed to update BACnet object\n");
        return -1;
    }
    
    return 0;
}

int message_handler_process_object_delete(
    const object_delete_msg_t *msg,
    uint32_t instance_offset)
{
    uint32_t instance;
    
    if (!msg) return -1;
    
    instance = msg->object_instance + instance_offset;
    
    /* Delete from cache */
    redis_cache_delete_object(msg->object_type, instance);
    
    /* Delete BACnet object */
    bacnet_server_delete_object(msg->object_type, instance);
    
    printf("[MSG] Deleted object (type=%d, instance=%u): %s\n",
           msg->object_type, instance, msg->reason);
    
    return 0;
}

int message_handler_process_device_config(const device_config_msg_t *msg)
{
    if (!msg) return -1;
    
    bacnet_server_update_device(
        msg->has_device_name ? msg->device_name : NULL,
        msg->has_device_description ? msg->device_description : NULL,
        msg->has_location ? msg->location : NULL
    );
    
    return 0;
}

const char* message_handler_error_string(parse_result_t result)
{
    switch (result) {
        case PARSE_OK: return "OK";
        case PARSE_ERROR_INVALID_JSON: return "Invalid JSON";
        case PARSE_ERROR_MISSING_FIELD: return "Missing required field";
        case PARSE_ERROR_INVALID_TYPE: return "Invalid type";
        case PARSE_ERROR_INVALID_VALUE: return "Invalid value";
        case PARSE_ERROR_UNKNOWN_MESSAGE_TYPE: return "Unknown message type";
        default: return "Unknown error";
    }
}
