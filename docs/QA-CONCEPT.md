# BACnet Event Server - Quality Assurance Concept

## Project Overview

The **BACnet Event Server** is a gateway that receives events from KurrentDB (EventStoreDB) via persistent subscriptions and exposes them as BACnet objects via BACnet/SC (Secure Connect) with Change-of-Value (CoV) support. A Redis cache provides persistence for crash recovery.

**Architecture:**
```
KurrentDB (Events) → BACnet Event Server → BACnet/SC Clients (BMS/SCADA)
                            ↓
                      Redis Cache
```

**Technology Stack:**
- C11 with POSIX threads
- libcurl (HTTP/JSON API for KurrentDB)
- cJSON (JSON parsing)
- hiredis (Redis client)
- BACnet-Stack (BACnet/SC protocol)
- OpenSSL/mbedTLS (TLS for BACnet/SC)

---

## Implementation Status

### ✅ Completed Features

| Feature | File(s) | Status |
|---------|---------|--------|
| Redis cache JSON parsing | `redis_cache.c` | ✅ Complete |
| ISO 8601 timestamp parsing | `message_handler.c` | ✅ Complete |
| Structured logging system | `logging.h`, `logging.c` | ✅ Complete |
| Health API & Prometheus metrics | `health_api.h`, `health_api.c` | ✅ Complete |
| BACnet/SC configuration | `bacnet_server.h` | ✅ Complete |

### 🔧 Simplified Implementations (Review Required)

#### 1. kurrentdb_client.c - Error Handling
**Location:** `src/kurrentdb_client.c:807`
```c
(void)action;  // Simplified - always retry
```
**Issue:** NAK action (skip, retry, park) is ignored; all errors result in retry.

**Review:** Implement proper error action handling per event.

---

#### 2. redis_cache.c - Value Update
**Location:** `src/redis_cache.c`
```c
/* Simplified here with full object update */
```
**Issue:** Value updates rewrite the entire object instead of using Redis hash field updates.

**Review:** Consider `HSET` for partial updates to reduce write amplification.

---

#### 3. bacnet_server.c - Statistics
**Location:** `src/bacnet_server.c:237`
```c
server->stats.read_requests++;  // Simplified - count all as reads
```
**Issue:** All BACnet requests are counted as reads; no differentiation between Read/Write/Subscribe.

**Review:** Implement proper request type classification.

---

## Logging System

### Features
- Multiple output targets: stdout, stderr, file, syslog, callback
- Log levels: TRACE, DEBUG, INFO, WARN, ERROR, FATAL
- Formats: Human-readable text, JSON (for log aggregation)
- Log rotation with configurable size and backup count
- ANSI color support for terminal output
- Thread-safe implementation

### Usage
```c
#include "logging.h"

// Initialize
log_config_t config;
log_get_default_config(&config);
config.level = LOG_LEVEL_DEBUG;
config.outputs = LOG_OUTPUT_STDOUT | LOG_OUTPUT_FILE;
config.file_path = "/var/log/bacnet-event-server/server.log";
config.format = LOG_FORMAT_JSON;  // For ELK/Loki
log_init(&config);

// Log messages
LOG_INFO("MAIN", "Server started on port %d", port);
LOG_ERROR("REDIS", "Connection failed: %s", error_msg);
LOG_DEBUG("BACNET", "Processing object %d:%d", type, instance);

// Structured logging (JSON)
log_structured(LOG_LEVEL_INFO, "METRICS", "Request processed",
    "{\"latency_ms\":42,\"object_type\":\"analog-input\"}");
```

### Log Aggregation Integration

**Loki (via Promtail):**
```yaml
# promtail-config.yml
scrape_configs:
  - job_name: bacnet-event-server
    static_configs:
      - targets: [localhost]
        labels:
          job: bacnet-event-server
          __path__: /var/log/bacnet-event-server/*.log
    pipeline_stages:
      - json:
          expressions:
            level: level
            component: component
            message: message
```

**ELK (via Filebeat):**
```yaml
# filebeat.yml
filebeat.inputs:
  - type: log
    paths:
      - /var/log/bacnet-event-server/*.log
    json.keys_under_root: true
    json.add_error_key: true
```

---

## Health API & Monitoring

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Overall health status (JSON) |
| `/health/live` | GET | Liveness probe (Kubernetes) |
| `/health/ready` | GET | Readiness probe (Kubernetes) |
| `/metrics` | GET | Prometheus metrics |
| `/status` | GET | Detailed system status (JSON) |

### Prometheus Metrics

```
# Process metrics
process_start_time_seconds
process_cpu_seconds_total
process_resident_memory_bytes
process_virtual_memory_bytes
process_open_fds

# Message processing
bacnet_messages_received_total
bacnet_messages_processed_total
bacnet_messages_failed_total
bacnet_message_processing_seconds_bucket{le="..."}
bacnet_message_processing_seconds_sum
bacnet_message_processing_seconds_count

# BACnet objects
bacnet_objects_total

# BACnet operations
bacnet_read_requests_total
bacnet_write_requests_total
bacnet_cov_notifications_total
bacnet_cov_subscriptions_active
bacnet_read_latency_seconds_bucket{le="..."}

# Redis
bacnet_redis_commands_total
bacnet_redis_errors_total
bacnet_redis_reconnects_total

# KurrentDB
bacnet_kurrentdb_events_received_total
bacnet_kurrentdb_events_acked_total
bacnet_kurrentdb_lag_events

# Errors
bacnet_errors_total
```

### Prometheus Configuration

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'bacnet-event-server'
    scrape_interval: 15s
    static_configs:
      - targets: ['bacnet-server:9090']
```

### Grafana Dashboard

Import dashboard from `config/grafana-dashboard.json` or use these panels:
1. Message Throughput (rate of messages_processed_total)
2. Processing Latency (histogram_quantile on processing_seconds)
3. Object Count (objects_total gauge)
4. Memory Usage (process_resident_memory_bytes)
5. Error Rate (rate of errors_total)
6. KurrentDB Lag (kurrentdb_lag_events gauge)

### Alerting Rules

```yaml
# prometheus-alerts.yml
groups:
  - name: bacnet-event-server
    rules:
      - alert: HighErrorRate
        expr: rate(bacnet_errors_total[5m]) > 0.1
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High error rate detected"
          
      - alert: HighMemoryUsage
        expr: process_resident_memory_bytes > 100000000
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Memory usage above 100MB"
          
      - alert: KurrentDBLagHigh
        expr: bacnet_kurrentdb_lag_events > 1000
        for: 2m
        labels:
          severity: critical
        annotations:
          summary: "KurrentDB subscription falling behind"
          
      - alert: ProcessingLatencyHigh
        expr: histogram_quantile(0.99, rate(bacnet_message_processing_seconds_bucket[5m])) > 0.1
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "p99 processing latency above 100ms"
```

### Kubernetes Integration

```yaml
# deployment.yaml
apiVersion: apps/v1
kind: Deployment
spec:
  template:
    spec:
      containers:
        - name: bacnet-event-server
          ports:
            - name: bacnet
              containerPort: 47808
              protocol: UDP
            - name: metrics
              containerPort: 9090
              protocol: TCP
          livenessProbe:
            httpGet:
              path: /health/live
              port: metrics
            initialDelaySeconds: 10
            periodSeconds: 10
          readinessProbe:
            httpGet:
              path: /health/ready
              port: metrics
            initialDelaySeconds: 5
            periodSeconds: 5
          resources:
            limits:
              memory: "128Mi"
              cpu: "500m"
```

---

## QA Focus Areas

### A. Memory Management
- [ ] Check all `malloc`/`calloc` for corresponding `free`
- [ ] Verify no memory leaks in error paths
- [ ] Review string buffer sizes (especially fixed-size arrays)
- [ ] Check for buffer overflows in `snprintf`/`strncpy` usage

### B. Thread Safety
- [ ] Verify mutex usage in `redis_cache.c` (global `g_ctx`)
- [ ] Review `kurrentdb_client.c` subscription thread lifecycle
- [ ] Check `bacnet_server.c` object list concurrent access
- [ ] Validate signal handler safety in `main.c`
- [ ] Review `logging.c` mutex usage
- [ ] Review `health_api.c` atomic operations

### C. Error Handling
- [ ] Verify all CURL calls check return codes
- [ ] Check Redis command error handling
- [ ] Review BACnet stack error propagation
- [ ] Validate JSON parse error recovery

### D. Resource Cleanup
- [ ] Verify proper shutdown sequence (subscriptions → server → cache)
- [ ] Check file descriptor cleanup
- [ ] Review thread join/cancel behavior
- [ ] Validate Redis connection pool cleanup
- [ ] Verify log file handle cleanup

### E. Integration Points
- [ ] KurrentDB reconnection logic under network failure
- [ ] Redis reconnection and state recovery
- [ ] BACnet CoV subscription persistence across restarts

### F. BACnet/SC Security
- [ ] TLS certificate validation (hub and peers)
- [ ] Private key protection and secure storage
- [ ] Certificate expiration handling
- [ ] WebSocket connection security
- [ ] Hub failover behavior
- [ ] Secure credential handling in config files

---

## File Overview

| File | Lines | Status | Notes |
|------|-------|--------|-------|
| `main.c` | 543 | Complete | Server lifecycle, config loading |
| `redis_cache.c` | 649 | ✅ Complete | JSON parsing implemented |
| `message_handler.c` | 719 | ✅ Complete | ISO 8601 timestamp parsing |
| `kurrentdb_client.c` | 917 | Review | Error handling simplified |
| `bacnet_server.c` | 971 | Review | Statistics simplified |
| `logging.c` | ~450 | ✅ New | Structured logging system |
| `health_api.c` | ~650 | ✅ New | Health checks & Prometheus metrics |

---

## Test Plan Reference

See `docs/TEST-PLAN.md` for:
- Durability test specification (24h, 100 msg/s)
- Docker resource constraints
- Load generator scripts
- BACnet client test scripts
- Prometheus/Grafana monitoring setup
- Test execution procedures
- Report template

---

## Build & Test

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)

# Run with Valgrind
valgrind --leak-check=full ./bacnet-event-server -c ../config/server-config.json

# Static analysis
cppcheck --enable=all ../src ../include

# Run durability test
docker-compose -f docker-compose.test.yml up -d
```

---

## License

EUPL-1.2 - Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
