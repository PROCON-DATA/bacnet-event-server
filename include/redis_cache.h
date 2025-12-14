/**
 * @file redis_cache.h
 * @brief Redis Cache Integration for BACnet Object Storage
 * 
 * Persistent cache for BACnet objects and values.
 * Enables recovery after server restart.
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#ifndef REDIS_CACHE_H
#define REDIS_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Data Types
 * ============================================================================ */

/**
 * @brief Redis connection configuration
 */
typedef struct redis_config {
    const char *host;
    int port;
    const char *password;        /* NULL if no password */
    int database;
    const char *key_prefix;
    int connection_timeout_ms;
    int command_timeout_ms;
} redis_config_t;

/**
 * @brief BACnet object types (simplified)
 */
typedef enum bacnet_object_type {
    OBJECT_ANALOG_INPUT = 0,
    OBJECT_ANALOG_OUTPUT = 1,
    OBJECT_ANALOG_VALUE = 2,
    OBJECT_BINARY_INPUT = 3,
    OBJECT_BINARY_OUTPUT = 4,
    OBJECT_BINARY_VALUE = 5,
    OBJECT_MULTI_STATE_INPUT = 13,
    OBJECT_MULTI_STATE_OUTPUT = 14,
    OBJECT_MULTI_STATE_VALUE = 19,
    OBJECT_TYPE_MAX
} bacnet_object_type_t;

/**
 * @brief Value types
 */
typedef enum value_type {
    VALUE_TYPE_REAL,
    VALUE_TYPE_UNSIGNED,
    VALUE_TYPE_SIGNED,
    VALUE_TYPE_BOOLEAN,
    VALUE_TYPE_ENUMERATED
} value_type_t;

/**
 * @brief Status flags
 */
typedef struct status_flags {
    bool in_alarm;
    bool fault;
    bool overridden;
    bool out_of_service;
} status_flags_t;

/**
 * @brief BACnet object definition in cache
 */
typedef struct cached_object {
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
    char state_texts[16][64];    /* For multi-state */
    int state_count;
    char inactive_text[64];      /* For binary */
    char active_text[64];
    bool supports_priority_array;
    
    /* Current state */
    union {
        float real_value;
        uint32_t unsigned_value;
        int32_t signed_value;
        bool boolean_value;
    } present_value;
    
    status_flags_t status_flags;
    uint8_t reliability;
    uint8_t event_state;
    time_t last_update;
    char source_id[128];
    uint64_t stream_position;
} cached_object_t;

/**
 * @brief Callback for object iteration
 */
typedef void (*object_iterator_callback)(const cached_object_t *object, void *user_data);

/* ============================================================================
 * Functions
 * ============================================================================ */

/**
 * @brief Initializes Redis connection
 * @param config Configuration
 * @return 0 on success, -1 on error
 */
int redis_cache_init(const redis_config_t *config);

/**
 * @brief Shuts down Redis connection
 */
void redis_cache_shutdown(void);

/**
 * @brief Checks if connection is active
 * @return true if connected
 */
bool redis_cache_is_connected(void);

/**
 * @brief Reconnect after connection loss
 * @return 0 on success
 */
int redis_cache_reconnect(void);

/**
 * @brief Stores or updates a BACnet object
 * @param object Object data
 * @return 0 on success
 */
int redis_cache_store_object(const cached_object_t *object);

/**
 * @brief Loads a BACnet object from cache
 * @param object_type Object type
 * @param object_instance Instance number
 * @param object Output buffer
 * @return 0 on success, -1 if not found
 */
int redis_cache_load_object(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    cached_object_t *object
);

/**
 * @brief Updates only the present value of an object
 * @param object_type Object type
 * @param object_instance Instance number
 * @param value_type Value type
 * @param value New value (pointer to corresponding type)
 * @param status_flags Status flags (can be NULL)
 * @param source_timestamp Source timestamp
 * @return 0 on success
 */
int redis_cache_update_value(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    value_type_t value_type,
    const void *value,
    const status_flags_t *status_flags,
    time_t source_timestamp
);

/**
 * @brief Deletes an object from cache
 * @param object_type Object type
 * @param object_instance Instance number
 * @return 0 on success
 */
int redis_cache_delete_object(
    bacnet_object_type_t object_type,
    uint32_t object_instance
);

/**
 * @brief Iterates over all objects of a type
 * @param object_type Object type (OBJECT_TYPE_MAX for all)
 * @param callback Callback function
 * @param user_data User data for callback
 * @return Number of iterated objects
 */
int redis_cache_iterate_objects(
    bacnet_object_type_t object_type,
    object_iterator_callback callback,
    void *user_data
);

/**
 * @brief Returns the number of cached objects
 * @param object_type Object type (OBJECT_TYPE_MAX for all)
 * @return Number of objects
 */
int redis_cache_object_count(bacnet_object_type_t object_type);

/**
 * @brief Stores the last processed stream position
 * @param subscription_id Subscription ID
 * @param position Stream position
 * @return 0 on success
 */
int redis_cache_store_stream_position(
    const char *subscription_id,
    uint64_t position
);

/**
 * @brief Loads the last processed stream position
 * @param subscription_id Subscription ID
 * @param position Output buffer for position
 * @return 0 on success, -1 if not found
 */
int redis_cache_load_stream_position(
    const char *subscription_id,
    uint64_t *position
);

/**
 * @brief Stores device configuration
 * @param device_instance Device instance
 * @param name Device name
 * @param description Description
 * @param location Location
 * @return 0 on success
 */
int redis_cache_store_device_config(
    uint32_t device_instance,
    const char *name,
    const char *description,
    const char *location
);

/**
 * @brief Loads device configuration
 * @param device_instance Device instance
 * @param name Name buffer (256 bytes)
 * @param description Description buffer (512 bytes)
 * @param location Location buffer (256 bytes)
 * @return 0 on success
 */
int redis_cache_load_device_config(
    uint32_t device_instance,
    char *name,
    char *description,
    char *location
);

/**
 * @brief Pub/Sub: Publishes value change event
 * @param object_type Object type
 * @param object_instance Instance number
 * @return 0 on success
 */
int redis_cache_publish_value_change(
    bacnet_object_type_t object_type,
    uint32_t object_instance
);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_CACHE_H */
