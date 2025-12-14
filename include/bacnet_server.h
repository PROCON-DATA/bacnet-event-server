/**
 * @file bacnet_server.h
 * @brief BACnet/SC Server Integration with COV Support
 * 
 * Integrates the BACnet stack with the Redis cache and
 * implements Change-of-Value (COV) notifications over
 * BACnet/SC (Secure Connect) with TLS encryption.
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#ifndef BACNET_SERVER_H
#define BACNET_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include "redis_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define BACNET_MAX_COV_SUBSCRIPTIONS 100
#define BACNET_DEFAULT_COV_LIFETIME 300  /* seconds */

/* ============================================================================
 * Data Types
 * ============================================================================ */

/**
 * @brief BACnet server configuration
 */
typedef struct bacnet_server_config {
    uint32_t device_instance;
    const char *device_name;
    const char *device_description;
    uint16_t vendor_id;
    const char *vendor_name;
    const char *model_name;
    const char *application_software_version;
    const char *location;
    
    /* BACnet/SC Configuration */
    const char *hub_uri;             /* Primary hub WebSocket URI */
    const char *failover_hub_uri;    /* Failover hub URI (optional) */
    const char *certificate_file;    /* Device certificate (PEM) */
    const char *private_key_file;    /* Private key (PEM) */
    const char *ca_certificate_file; /* CA certificate for hub verification */
    bool hub_function_enabled;       /* Act as hub (default: false, node only) */
    
    /* Legacy BACnet/IP (fallback) */
    int port;                        /* UDP port (default: 47808) */
    const char *interface;           /* Network interface */
    const char *broadcast_address;   /* Optional */
    
    /* COV */
    uint32_t cov_lifetime;           /* Default COV lifetime */
    int max_cov_subscriptions;
} bacnet_server_config_t;

/**
 * @brief COV subscription info
 */
typedef struct cov_subscription {
    uint32_t subscriber_process_id;
    uint8_t subscriber_address[6];  /* BACnet MAC address */
    bacnet_object_type_t object_type;
    uint32_t object_instance;
    bool confirmed;                 /* Confirmed vs unconfirmed */
    uint32_t lifetime;              /* Remaining lifetime */
    float cov_increment;            /* For analog objects */
    time_t created_at;
    time_t last_notification;
} cov_subscription_t;

/**
 * @brief Callback when write request comes from BACnet client
 */
typedef bool (*write_request_callback)(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    uint8_t property_id,
    const void *value,
    void *user_data
);

/**
 * @brief Server statistics
 */
typedef struct server_stats {
    uint64_t read_requests;
    uint64_t write_requests;
    uint64_t cov_notifications_sent;
    uint64_t cov_subscriptions_active;
    uint64_t objects_total;
    time_t uptime_start;
} server_stats_t;

/* ============================================================================
 * Functions - Server Lifecycle
 * ============================================================================ */

/**
 * @brief Initializes BACnet server
 * @param config Server configuration
 * @return 0 on success, -1 on error
 */
int bacnet_server_init(const bacnet_server_config_t *config);

/**
 * @brief Starts BACnet server (non-blocking)
 * @return 0 on success
 */
int bacnet_server_start(void);

/**
 * @brief Stops BACnet server
 */
void bacnet_server_stop(void);

/**
 * @brief Shuts down BACnet server and releases resources
 */
void bacnet_server_shutdown(void);

/**
 * @brief Processes BACnet messages (must be called regularly)
 * @param timeout_ms Timeout in milliseconds
 * @return Number of processed messages
 */
int bacnet_server_task(int timeout_ms);

/**
 * @brief Returns server statistics
 * @param stats Output buffer
 */
void bacnet_server_get_stats(server_stats_t *stats);

/* ============================================================================
 * Functions - Object Management
 * ============================================================================ */

/**
 * @brief Creates or updates a BACnet object from cache data
 * @param object Object data from Redis cache
 * @return 0 on success
 */
int bacnet_server_create_object(const cached_object_t *object);

/**
 * @brief Updates the present value of an object
 * @param object_type Object type
 * @param object_instance Instance number
 * @param value_type Value type
 * @param value New value
 * @param status_flags Status flags (optional)
 * @return 0 on success
 * 
 * This function automatically triggers COV notifications when needed.
 */
int bacnet_server_update_value(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    value_type_t value_type,
    const void *value,
    const status_flags_t *status_flags
);

/**
 * @brief Deletes a BACnet object
 * @param object_type Object type
 * @param object_instance Instance number
 * @return 0 on success
 */
int bacnet_server_delete_object(
    bacnet_object_type_t object_type,
    uint32_t object_instance
);

/**
 * @brief Loads all objects from Redis cache
 * @return Number of loaded objects, -1 on error
 */
int bacnet_server_load_from_cache(void);

/**
 * @brief Updates device properties
 * @param name New name (NULL = no change)
 * @param description New description (NULL = no change)
 * @param location New location (NULL = no change)
 * @return 0 on success
 */
int bacnet_server_update_device(
    const char *name,
    const char *description,
    const char *location
);

/* ============================================================================
 * Functions - COV (Change of Value)
 * ============================================================================ */

/**
 * @brief Checks if value change should trigger COV notification
 * @param object_type Object type
 * @param object_instance Instance number
 * @param old_value Old value
 * @param new_value New value
 * @param cov_increment COV increment (for analog values)
 * @return true if COV should be triggered
 */
bool bacnet_cov_check_change(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    const void *old_value,
    const void *new_value,
    float cov_increment
);

/**
 * @brief Sends COV notification to all subscribers
 * @param object_type Object type
 * @param object_instance Instance number
 * @return Number of sent notifications
 */
int bacnet_cov_send_notifications(
    bacnet_object_type_t object_type,
    uint32_t object_instance
);

/**
 * @brief Returns active COV subscriptions for an object
 * @param object_type Object type
 * @param object_instance Instance number
 * @param subscriptions Output array
 * @param max_subscriptions Maximum size of array
 * @return Number of subscriptions
 */
int bacnet_cov_get_subscriptions(
    bacnet_object_type_t object_type,
    uint32_t object_instance,
    cov_subscription_t *subscriptions,
    int max_subscriptions
);

/**
 * @brief Updates COV lifetimes (must be called regularly)
 * @param elapsed_seconds Elapsed time since last call
 * @return Number of expired subscriptions
 */
int bacnet_cov_update_lifetimes(int elapsed_seconds);

/**
 * @brief Sets callback for external write requests
 * @param callback Callback function
 * @param user_data User data
 */
void bacnet_server_set_write_callback(
    write_request_callback callback,
    void *user_data
);

/* ============================================================================
 * Functions - Utilities
 * ============================================================================ */

/**
 * @brief Converts object type string to enum
 * @param type_str Object type as string (e.g. "analog-input")
 * @return Object type enum or -1 on error
 */
bacnet_object_type_t bacnet_object_type_from_string(const char *type_str);

/**
 * @brief Converts object type enum to string
 * @param object_type Object type
 * @return String representation
 */
const char* bacnet_object_type_to_string(bacnet_object_type_t object_type);

/**
 * @brief Converts value type string to enum
 * @param type_str Value type as string
 * @return Value type enum or -1 on error
 */
value_type_t bacnet_value_type_from_string(const char *type_str);

#ifdef __cplusplus
}
#endif

#endif /* BACNET_SERVER_H */
