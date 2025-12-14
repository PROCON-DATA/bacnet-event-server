/**
 * @file message_handler.h
 * @brief JSON Message Handler for KurrentDB Events
 * 
 * Parses JSON messages from KurrentDB and maps them to
 * BACnet objects and values.
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#ifndef MESSAGE_HANDLER_H
#define MESSAGE_HANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "redis_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Data Types
 * ============================================================================ */

/**
 * @brief Message types according to JSON schema
 */
typedef enum message_type {
    MSG_TYPE_UNKNOWN = 0,
    MSG_TYPE_OBJECT_DEFINITION,
    MSG_TYPE_VALUE_UPDATE,
    MSG_TYPE_OBJECT_DELETE,
    MSG_TYPE_DEVICE_CONFIG
} message_type_t;

/**
 * @brief Parsed object definition payload
 */
typedef struct object_definition_msg {
    bacnet_object_type_t object_type;
    uint32_t object_instance;
    char object_name[256];
    char description[512];
    value_type_t value_type;
    uint32_t units;
    char units_text[64];
    float cov_increment;
    float min_value;
    float max_value;
    char state_texts[16][64];
    int state_count;
    char inactive_text[64];
    char active_text[64];
    bool supports_priority_array;
    bool has_initial_value;
    union {
        float real_value;
        uint32_t unsigned_value;
        int32_t signed_value;
        bool boolean_value;
    } initial_value;
} object_definition_msg_t;

/**
 * @brief Parsed value update payload
 */
typedef struct value_update_msg {
    bacnet_object_type_t object_type;
    uint32_t object_instance;
    value_type_t value_type;
    union {
        float real_value;
        uint32_t unsigned_value;
        int32_t signed_value;
        bool boolean_value;
    } present_value;
    char quality[16];
    status_flags_t status_flags;
    bool has_status_flags;
    uint8_t reliability;
    bool has_reliability;
    uint8_t event_state;
    bool has_event_state;
    uint8_t priority;
    bool has_priority;
    int64_t source_timestamp_ms;
    bool has_source_timestamp;
} value_update_msg_t;

/**
 * @brief Parsed object delete payload
 */
typedef struct object_delete_msg {
    bacnet_object_type_t object_type;
    uint32_t object_instance;
    char reason[256];
} object_delete_msg_t;

/**
 * @brief Parsed device config payload
 */
typedef struct device_config_msg {
    char device_name[256];
    bool has_device_name;
    char device_description[512];
    bool has_device_description;
    char location[256];
    bool has_location;
    char model_name[256];
    bool has_model_name;
    char vendor_name[256];
    bool has_vendor_name;
    char application_version[64];
    bool has_application_version;
} device_config_msg_t;

/**
 * @brief Parsed message (union of all payload types)
 */
typedef struct parsed_message {
    message_type_t type;
    char source_id[128];
    int64_t timestamp_ms;
    uint64_t stream_position;
    char correlation_id[64];
    
    union {
        object_definition_msg_t object_definition;
        value_update_msg_t value_update;
        object_delete_msg_t object_delete;
        device_config_msg_t device_config;
    } payload;
} parsed_message_t;

/**
 * @brief Parse result
 */
typedef enum parse_result {
    PARSE_OK = 0,
    PARSE_ERROR_INVALID_JSON,
    PARSE_ERROR_MISSING_FIELD,
    PARSE_ERROR_INVALID_TYPE,
    PARSE_ERROR_INVALID_VALUE,
    PARSE_ERROR_UNKNOWN_MESSAGE_TYPE
} parse_result_t;

/* ============================================================================
 * Functions
 * ============================================================================ */

/**
 * @brief Initializes message handler
 * @return 0 on success
 */
int message_handler_init(void);

/**
 * @brief Shuts down message handler
 */
void message_handler_shutdown(void);

/**
 * @brief Parses a JSON message
 * @param json JSON string
 * @param json_length Length of JSON string
 * @param message Output for parsed message
 * @return Parse result
 */
parse_result_t message_handler_parse(
    const char *json,
    size_t json_length,
    parsed_message_t *message
);

/**
 * @brief Processes a parsed message completely
 * 
 * This function:
 * 1. Updates the Redis cache
 * 2. Updates the BACnet object
 * 3. Triggers COV notifications when needed
 * 
 * @param message Parsed message
 * @param subscription_id Subscription ID (for offset calculation)
 * @param instance_offset Object instance offset
 * @return 0 on success, -1 on error
 */
int message_handler_process(
    const parsed_message_t *message,
    const char *subscription_id,
    uint32_t instance_offset
);

/**
 * @brief Processes ObjectDefinition message
 * @param msg Object definition
 * @param instance_offset Offset for instance number
 * @return 0 on success
 */
int message_handler_process_object_definition(
    const object_definition_msg_t *msg,
    uint32_t instance_offset
);

/**
 * @brief Processes ValueUpdate message
 * @param msg Value update
 * @param instance_offset Offset for instance number
 * @param source_id Source ID
 * @return 0 on success
 */
int message_handler_process_value_update(
    const value_update_msg_t *msg,
    uint32_t instance_offset,
    const char *source_id
);

/**
 * @brief Processes ObjectDelete message
 * @param msg Object delete
 * @param instance_offset Offset for instance number
 * @return 0 on success
 */
int message_handler_process_object_delete(
    const object_delete_msg_t *msg,
    uint32_t instance_offset
);

/**
 * @brief Processes DeviceConfig message
 * @param msg Device config
 * @return 0 on success
 */
int message_handler_process_device_config(
    const device_config_msg_t *msg
);

/**
 * @brief Returns parse error as string
 * @param result Parse result
 * @return Error text
 */
const char* message_handler_error_string(parse_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_HANDLER_H */
