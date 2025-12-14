# BACnet Event Server - Test Plan

## Durability Test Specification

### Overview

This test plan defines a durability test scenario for the BACnet Event Server under constrained resources with high message throughput and concurrent BACnet read operations.

### Test Objectives

1. **Stability**: Verify system stability over extended runtime (24h+)
2. **Performance**: Validate 100 messages/second throughput
3. **Resource Limits**: Confirm operation within Docker resource constraints
4. **Memory Leaks**: Detect memory leaks via continuous monitoring
5. **Recovery**: Test automatic recovery after component failures

---

## Test Environment

### Docker Resource Constraints

```yaml
# docker-compose.test.yml
services:
  bacnet-server:
    deploy:
      resources:
        limits:
          cpus: '0.5'        # 50% of one CPU core
          memory: 128M       # 128 MB RAM limit
        reservations:
          cpus: '0.25'
          memory: 64M
    ulimits:
      nofile:
        soft: 1024
        hard: 2048
```

### Infrastructure Components

| Component | Resource Limit | Purpose |
|-----------|---------------|---------|
| bacnet-server | 0.5 CPU, 128MB RAM | System under test |
| kurrentdb | 1.0 CPU, 512MB RAM | Event source |
| redis | 0.25 CPU, 64MB RAM | Cache layer |
| bacnet-client | 0.25 CPU, 64MB RAM | Test client |
| load-generator | 0.5 CPU, 128MB RAM | Event generator |
| prometheus | 0.25 CPU, 128MB RAM | Metrics collection |
| grafana | 0.25 CPU, 128MB RAM | Visualization |

---

## Test Scenarios

### Scenario 1: Sustained Load (100 msg/s)

**Duration:** 24 hours

**Load Profile:**
```
Messages per second: 100
Message types:
  - ValueUpdate: 90%
  - ObjectDefinition: 8%
  - ObjectDelete: 2%

Object distribution:
  - Analog Input: 40%
  - Analog Value: 30%
  - Binary Input: 20%
  - Multi-State Value: 10%

Total objects: 1000
```

**BACnet Read Operations:**
```
Read requests per second: 50
Distribution:
  - ReadProperty: 60%
  - ReadPropertyMultiple: 30%
  - ReadRange: 10%

Target selection: Random (uniform distribution)
```

**Success Criteria:**
- [ ] No OOM kills
- [ ] CPU usage < 80% average
- [ ] Memory usage stable (no continuous growth)
- [ ] Message processing latency p99 < 100ms
- [ ] BACnet read latency p99 < 50ms
- [ ] Zero message loss
- [ ] Zero unhandled exceptions

---

### Scenario 2: Burst Load

**Duration:** 4 hours

**Load Profile:**
```
Base load: 50 msg/s
Burst periods: Every 10 minutes
Burst duration: 30 seconds
Burst rate: 500 msg/s
```

**Success Criteria:**
- [ ] System recovers within 60 seconds after burst
- [ ] No message loss during burst
- [ ] Queue depth returns to normal after burst

---

### Scenario 3: Component Failure Recovery

**Duration:** 8 hours

**Failure Injection:**
```
Every 30 minutes, inject one of:
  - Redis restart (30% probability)
  - KurrentDB restart (30% probability)
  - Network partition (20% probability)
  - BACnet client disconnect (20% probability)
```

**Success Criteria:**
- [ ] Automatic reconnection within 30 seconds
- [ ] State recovery from Redis after restart
- [ ] Catch-up from KurrentDB after reconnection
- [ ] CoV subscriptions restored after client reconnect

---

### Scenario 4: Memory Stress

**Duration:** 12 hours

**Configuration:**
```
Memory limit: 64MB (reduced from 128MB)
Message rate: 100 msg/s
Object count: 2000 (doubled)
```

**Success Criteria:**
- [ ] No OOM kills
- [ ] Graceful degradation if memory pressure
- [ ] Proper cleanup of old objects

---

## Test Implementation

### Load Generator Script

```python
#!/usr/bin/env python3
# tests/load_generator.py

import asyncio
import json
import random
import time
from datetime import datetime, timezone
from esdbclient import EventStoreDBClient

OBJECT_TYPES = [
    ("analog-input", 0.4),
    ("analog-value", 0.3),
    ("binary-input", 0.2),
    ("multi-state-value", 0.1)
]

MESSAGE_TYPES = [
    ("ValueUpdate", 0.90),
    ("ObjectDefinition", 0.08),
    ("ObjectDelete", 0.02)
]

class LoadGenerator:
    def __init__(self, connection_string: str, target_rate: int = 100):
        self.client = EventStoreDBClient(uri=connection_string)
        self.target_rate = target_rate
        self.objects = {}  # Track created objects
        self.stats = {
            "sent": 0,
            "errors": 0,
            "start_time": time.time()
        }
    
    def weighted_choice(self, choices):
        r = random.random()
        cumulative = 0
        for choice, weight in choices:
            cumulative += weight
            if r <= cumulative:
                return choice
        return choices[-1][0]
    
    def generate_value_update(self, obj_type: str, instance: int) -> dict:
        if obj_type.startswith("analog"):
            value = random.uniform(0, 100)
        elif obj_type.startswith("binary"):
            value = random.choice([True, False])
        else:
            value = random.randint(1, 5)
        
        return {
            "messageType": "ValueUpdate",
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "sourceId": f"load-generator",
            "payload": {
                "objectType": obj_type,
                "objectInstance": instance,
                "presentValue": value,
                "quality": "good",
                "statusFlags": {
                    "inAlarm": False,
                    "fault": False,
                    "overridden": False,
                    "outOfService": False
                }
            }
        }
    
    def generate_object_definition(self, obj_type: str, instance: int) -> dict:
        return {
            "messageType": "ObjectDefinition",
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "sourceId": f"load-generator",
            "payload": {
                "objectType": obj_type,
                "objectInstance": instance,
                "objectName": f"Test_{obj_type}_{instance}",
                "description": f"Load test object {instance}",
                "presentValueType": "real" if "analog" in obj_type else "boolean",
                "units": 95,
                "covIncrement": 0.1
            }
        }
    
    async def generate_message(self) -> dict:
        msg_type = self.weighted_choice(MESSAGE_TYPES)
        obj_type = self.weighted_choice(OBJECT_TYPES)
        
        if msg_type == "ObjectDefinition":
            instance = len(self.objects) + 1
            self.objects[(obj_type, instance)] = True
            return self.generate_object_definition(obj_type, instance)
        
        elif msg_type == "ValueUpdate":
            if not self.objects:
                # Create first object
                instance = 1
                self.objects[(obj_type, instance)] = True
                return self.generate_object_definition(obj_type, instance)
            
            # Update random existing object
            obj_key = random.choice(list(self.objects.keys()))
            return self.generate_value_update(obj_key[0], obj_key[1])
        
        else:  # ObjectDelete
            if len(self.objects) > 10:
                obj_key = random.choice(list(self.objects.keys()))
                del self.objects[obj_key]
                return {
                    "messageType": "ObjectDelete",
                    "timestamp": datetime.now(timezone.utc).isoformat(),
                    "sourceId": "load-generator",
                    "payload": {
                        "objectType": obj_key[0],
                        "objectInstance": obj_key[1],
                        "reason": "load-test-cleanup"
                    }
                }
            else:
                # Not enough objects, send value update instead
                obj_key = random.choice(list(self.objects.keys()))
                return self.generate_value_update(obj_key[0], obj_key[1])
    
    async def run(self, duration_seconds: int = None):
        interval = 1.0 / self.target_rate
        end_time = time.time() + duration_seconds if duration_seconds else None
        
        while True:
            if end_time and time.time() >= end_time:
                break
            
            start = time.time()
            
            try:
                message = await self.generate_message()
                event_data = json.dumps(message).encode('utf-8')
                
                self.client.append_to_stream(
                    stream_name="energy-meters",
                    events=[{
                        "type": message["messageType"],
                        "data": event_data
                    }]
                )
                self.stats["sent"] += 1
                
            except Exception as e:
                self.stats["errors"] += 1
                print(f"Error: {e}")
            
            elapsed = time.time() - start
            if elapsed < interval:
                await asyncio.sleep(interval - elapsed)
            
            # Print stats every 10 seconds
            if self.stats["sent"] % (self.target_rate * 10) == 0:
                rate = self.stats["sent"] / (time.time() - self.stats["start_time"])
                print(f"Sent: {self.stats['sent']}, Rate: {rate:.1f}/s, Errors: {self.stats['errors']}")

if __name__ == "__main__":
    import sys
    rate = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else None
    
    generator = LoadGenerator("esdb://localhost:2113?tls=false", rate)
    asyncio.run(generator.run(duration))
```

### BACnet Read Client Script

```python
#!/usr/bin/env python3
# tests/bacnet_reader.py

import asyncio
import random
import time
import BAC0

class BACnetReader:
    def __init__(self, device_address: str, device_instance: int):
        self.device_address = device_address
        self.device_instance = device_instance
        self.bacnet = None
        self.stats = {
            "reads": 0,
            "errors": 0,
            "latencies": []
        }
    
    async def connect(self):
        self.bacnet = BAC0.lite()
        await asyncio.sleep(2)  # Wait for network discovery
    
    async def read_property(self, obj_type: str, instance: int, prop: str = "presentValue"):
        start = time.time()
        try:
            result = self.bacnet.read(
                f"{self.device_address} {obj_type} {instance} {prop}"
            )
            latency = (time.time() - start) * 1000
            self.stats["reads"] += 1
            self.stats["latencies"].append(latency)
            return result, latency
        except Exception as e:
            self.stats["errors"] += 1
            return None, 0
    
    async def random_read(self, max_instance: int = 1000):
        obj_types = ["analogInput", "analogValue", "binaryInput", "multiStateValue"]
        obj_type = random.choice(obj_types)
        instance = random.randint(1, max_instance)
        return await self.read_property(obj_type, instance)
    
    async def run(self, rate: int = 50, duration_seconds: int = None):
        await self.connect()
        interval = 1.0 / rate
        end_time = time.time() + duration_seconds if duration_seconds else None
        
        while True:
            if end_time and time.time() >= end_time:
                break
            
            start = time.time()
            await self.random_read()
            
            elapsed = time.time() - start
            if elapsed < interval:
                await asyncio.sleep(interval - elapsed)
            
            if self.stats["reads"] % (rate * 10) == 0:
                avg_latency = sum(self.stats["latencies"][-100:]) / min(100, len(self.stats["latencies"]))
                print(f"Reads: {self.stats['reads']}, Avg latency: {avg_latency:.1f}ms, Errors: {self.stats['errors']}")

if __name__ == "__main__":
    reader = BACnetReader("192.168.1.100", 1234)
    asyncio.run(reader.run(rate=50))
```

---

## Monitoring Setup

### Prometheus Metrics (scraped from /metrics endpoint)

```yaml
# prometheus.yml
scrape_configs:
  - job_name: 'bacnet-event-server'
    scrape_interval: 5s
    static_configs:
      - targets: ['bacnet-server:9090']
```

### Key Metrics to Monitor

| Metric | Type | Alert Threshold |
|--------|------|-----------------|
| `bacnet_messages_processed_total` | Counter | - |
| `bacnet_messages_processing_seconds` | Histogram | p99 > 100ms |
| `bacnet_objects_total` | Gauge | - |
| `bacnet_cov_subscriptions_active` | Gauge | - |
| `bacnet_redis_operations_total` | Counter | - |
| `bacnet_redis_latency_seconds` | Histogram | p99 > 10ms |
| `bacnet_kurrentdb_lag_events` | Gauge | > 1000 |
| `bacnet_read_requests_total` | Counter | - |
| `bacnet_read_latency_seconds` | Histogram | p99 > 50ms |
| `process_resident_memory_bytes` | Gauge | > 120MB |
| `process_cpu_seconds_total` | Counter | - |

### Grafana Dashboard Panels

1. **Message Throughput** - messages/second over time
2. **Processing Latency** - p50, p95, p99 latencies
3. **Object Count** - total BACnet objects
4. **Memory Usage** - RSS vs limit
5. **CPU Usage** - percentage of limit
6. **Error Rate** - errors/minute
7. **KurrentDB Lag** - events behind
8. **Redis Operations** - ops/second
9. **BACnet Reads** - reads/second with latency

---

## Test Execution

### Pre-Test Checklist

- [ ] Docker environment clean (no old containers/volumes)
- [ ] Resource limits configured correctly
- [ ] Prometheus/Grafana accessible
- [ ] Load generator tested at low rate
- [ ] BACnet client connectivity verified
- [ ] Logging configured to capture errors

### Test Commands

```bash
# Start test environment
docker-compose -f docker-compose.test.yml up -d

# Start load generator (100 msg/s for 24 hours)
docker exec -it load-generator python /tests/load_generator.py 100 86400

# Start BACnet reader (50 reads/s)
docker exec -it bacnet-client python /tests/bacnet_reader.py

# Monitor logs
docker-compose -f docker-compose.test.yml logs -f bacnet-server

# Check resource usage
docker stats

# Export metrics
curl http://localhost:9090/metrics > metrics_$(date +%Y%m%d_%H%M%S).txt
```

### Post-Test Analysis

1. **Export Prometheus data** for the test period
2. **Analyze memory trend** - any continuous growth indicates leak
3. **Calculate success rate** - messages sent vs processed
4. **Review error logs** - categorize any errors
5. **Generate test report** with all metrics

---

## Test Report Template

```markdown
# Durability Test Report

**Test Date:** YYYY-MM-DD
**Duration:** XX hours
**Configuration:** [Scenario Name]

## Summary
- **Result:** PASS / FAIL
- **Messages Processed:** X,XXX,XXX
- **Error Rate:** X.XX%
- **Peak Memory:** XXX MB
- **Average CPU:** XX%

## Metrics Summary
| Metric | Min | Avg | P95 | P99 | Max |
|--------|-----|-----|-----|-----|-----|
| Processing Latency (ms) | | | | | |
| Read Latency (ms) | | | | | |
| Memory (MB) | | | | | |
| CPU (%) | | | | | |

## Issues Found
1. [Issue description]
2. [Issue description]

## Recommendations
1. [Recommendation]
2. [Recommendation]
```

---

## Appendix: Docker Compose Test Configuration

```yaml
# docker-compose.test.yml
version: '3.8'

services:
  redis:
    image: redis:7-alpine
    deploy:
      resources:
        limits:
          cpus: '0.25'
          memory: 64M
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 5s
      timeout: 3s
      retries: 3

  kurrentdb:
    image: eventstore/eventstore:23.10.0-jammy
    deploy:
      resources:
        limits:
          cpus: '1.0'
          memory: 512M
    environment:
      - EVENTSTORE_INSECURE=true
      - EVENTSTORE_RUN_PROJECTIONS=All
      - EVENTSTORE_ENABLE_ATOM_PUB_OVER_HTTP=true
    healthcheck:
      test: ["CMD", "curl", "-f", "http://localhost:2113/health/live"]
      interval: 5s
      timeout: 3s
      retries: 5

  bacnet-server:
    build: .
    deploy:
      resources:
        limits:
          cpus: '0.5'
          memory: 128M
        reservations:
          cpus: '0.25'
          memory: 64M
    depends_on:
      redis:
        condition: service_healthy
      kurrentdb:
        condition: service_healthy
    environment:
      - LOG_LEVEL=info
      - METRICS_PORT=9090
    ports:
      - "47808:47808/udp"
      - "9090:9090"

  load-generator:
    image: python:3.11-slim
    deploy:
      resources:
        limits:
          cpus: '0.5'
          memory: 128M
    volumes:
      - ./tests:/tests
    command: ["python", "/tests/load_generator.py", "100"]
    depends_on:
      - kurrentdb

  prometheus:
    image: prom/prometheus:latest
    deploy:
      resources:
        limits:
          cpus: '0.25'
          memory: 128M
    volumes:
      - ./config/prometheus.yml:/etc/prometheus/prometheus.yml
    ports:
      - "9091:9090"

  grafana:
    image: grafana/grafana:latest
    deploy:
      resources:
        limits:
          cpus: '0.25'
          memory: 128M
    ports:
      - "3000:3000"
    environment:
      - GF_SECURITY_ADMIN_PASSWORD=admin
```
