/**
 * @file kurrentdb_client.h
 * @brief KurrentDB (EventStoreDB) Client Integration
 * 
 * Handles subscriptions to KurrentDB streams and processes
 * incoming events for BACnet objects.
 * 
 * SPDX-License-Identifier: EUPL-1.2
 * Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
 */

#ifndef KURRENTDB_CLIENT_H
#define KURRENTDB_CLIENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================ */

#define KURRENTDB_MAX_SUBSCRIPTIONS 32
#define KURRENTDB_MAX_STREAM_NAME 256
#define KURRENTDB_MAX_GROUP_NAME 128
#define KURRENTDB_MAX_EVENT_TYPE 256

/* ============================================================================
 * Data Types
 * ============================================================================ */

/**
 * @brief KurrentDB connection configuration
 */
typedef struct kurrentdb_config {
    const char *connection_string;  /* esdb://host:port?options */
    bool tls_enabled;
    bool tls_verify_cert;
    const char *tls_ca_file;        /* CA certificate file */
    int reconnect_delay_ms;
    int max_reconnect_attempts;     /* -1 for infinite */
} kurrentdb_config_t;

/**
 * @brief Subscription start position
 */
typedef enum subscription_start {
    SUBSCRIPTION_START_BEGIN,       /* From beginning of stream */
    SUBSCRIPTION_START_END,         /* From current end */
    SUBSCRIPTION_START_POSITION     /* From specific position */
} subscription_start_t;

/**
 * @brief Subscription configuration
 */
typedef struct subscription_config {
    const char *subscription_id;
    const char *stream_name;
    const char *group_name;         /* For persistent subscriptions */
    subscription_start_t start_from;
    uint64_t start_position;        /* When start_from == POSITION */
    uint32_t object_instance_offset;
    bool enabled;
} subscription_config_t;

/**
 * @brief Event metadata
 */
typedef struct event_metadata {
    char event_id[64];
    char event_type[KURRENTDB_MAX_EVENT_TYPE];
    char stream_id[KURRENTDB_MAX_STREAM_NAME];
    uint64_t stream_revision;
    uint64_t commit_position;
    uint64_t prepare_position;
    uint64_t created_timestamp;     /* Unix timestamp in ms */
} event_metadata_t;

/**
 * @brief Received event
 */
typedef struct received_event {
    event_metadata_t metadata;
    const char *data;               /* JSON payload */
    size_t data_length;
    const char *subscription_id;
} received_event_t;

/**
 * @brief Callback for received events
 * @param event The received event
 * @param user_data User-defined data
 * @return true if event successfully processed, false for NAK
 */
typedef bool (*event_received_callback)(
    const received_event_t *event,
    void *user_data
);

/**
 * @brief Callback for subscription errors
 * @param subscription_id Subscription ID
 * @param error_message Error message
 * @param user_data User-defined data
 */
typedef void (*subscription_error_callback)(
    const char *subscription_id,
    const char *error_message,
    void *user_data
);

/**
 * @brief Callback for connection status
 * @param connected true if connected
 * @param user_data User-defined data
 */
typedef void (*connection_status_callback)(
    bool connected,
    void *user_data
);

/**
 * @brief Subscription handle
 */
typedef struct kurrentdb_subscription* kurrentdb_subscription_handle;

/* ============================================================================
 * Functions - Connection
 * ============================================================================ */

/**
 * @brief Initializes KurrentDB client
 * @param config Connection configuration
 * @return 0 on success, -1 on error
 */
int kurrentdb_init(const kurrentdb_config_t *config);

/**
 * @brief Shuts down KurrentDB client and all subscriptions
 */
void kurrentdb_shutdown(void);

/**
 * @brief Checks connection status
 * @return true if connected
 */
bool kurrentdb_is_connected(void);

/**
 * @brief Sets callback for connection status
 * @param callback Callback function
 * @param user_data User-defined data
 */
void kurrentdb_set_connection_callback(
    connection_status_callback callback,
    void *user_data
);

/* ============================================================================
 * Functions - Subscriptions
 * ============================================================================ */

/**
 * @brief Creates a new subscription
 * @param config Subscription configuration
 * @param event_callback Callback for events
 * @param error_callback Callback for errors (optional)
 * @param user_data User-defined data
 * @return Subscription handle or NULL on error
 */
kurrentdb_subscription_handle kurrentdb_subscribe(
    const subscription_config_t *config,
    event_received_callback event_callback,
    subscription_error_callback error_callback,
    void *user_data
);

/**
 * @brief Creates a persistent subscription (catch-up after reconnect)
 * @param config Subscription configuration
 * @param event_callback Callback for events
 * @param error_callback Callback for errors (optional)
 * @param user_data User-defined data
 * @return Subscription handle or NULL on error
 */
kurrentdb_subscription_handle kurrentdb_subscribe_persistent(
    const subscription_config_t *config,
    event_received_callback event_callback,
    subscription_error_callback error_callback,
    void *user_data
);

/**
 * @brief Terminates a subscription
 * @param handle Subscription handle
 */
void kurrentdb_unsubscribe(kurrentdb_subscription_handle handle);

/**
 * @brief Acknowledges event processing (for persistent subscriptions)
 * @param handle Subscription handle
 * @param event_id Event ID
 * @return 0 on success
 */
int kurrentdb_ack_event(
    kurrentdb_subscription_handle handle,
    const char *event_id
);

/**
 * @brief Rejects event processing (NAK)
 * @param handle Subscription handle
 * @param event_id Event ID
 * @param reason Reason for NAK (optional)
 * @return 0 on success
 */
int kurrentdb_nak_event(
    kurrentdb_subscription_handle handle,
    const char *event_id,
    const char *reason
);

/**
 * @brief Returns the current position of a subscription
 * @param handle Subscription handle
 * @return Current stream position
 */
uint64_t kurrentdb_get_subscription_position(kurrentdb_subscription_handle handle);

/**
 * @brief Returns subscription ID
 * @param handle Subscription handle
 * @return Subscription ID string
 */
const char* kurrentdb_get_subscription_id(kurrentdb_subscription_handle handle);

/* ============================================================================
 * Functions - Event Loop
 * ============================================================================ */

/**
 * @brief Processes pending events (non-blocking)
 * @param timeout_ms Maximum wait time in ms (0 = check only)
 * @return Number of processed events, -1 on error
 */
int kurrentdb_poll_events(int timeout_ms);

/**
 * @brief Starts event loop in separate thread
 * @return 0 on success
 */
int kurrentdb_start_event_loop(void);

/**
 * @brief Stops event loop thread
 */
void kurrentdb_stop_event_loop(void);

#ifdef __cplusplus
}
#endif

#endif /* KURRENTDB_CLIENT_H */
