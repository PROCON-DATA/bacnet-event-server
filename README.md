# BACnet Event Server

A gateway server that receives measurement values from KurrentDB (EventStoreDB) and exposes them as BACnet objects via BACnet/SC (Secure Connect) with CoV (Change-of-Value) support.

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                                                                             │
│   ┌──────────────┐     ┌─────────────────┐     ┌──────────────────────┐   │
│   │              │     │                 │     │                      │   │
│   │  KurrentDB   │────▶│  BACnet Event   │────▶│   BACnet/SC Client  │   │
│   │  (Events)    │     │   Server        │     │   (BMS, SCADA)      │   │
│   │              │     │                 │     │                      │   │
│   └──────────────┘     └────────┬────────┘     └──────────────────────┘   │
│                                 │                                          │
│                                 │                                          │
│                                 ▼                                          │
│                        ┌─────────────────┐                                 │
│                        │                 │                                 │
│                        │  Redis Cache    │◀──── Spring Boot Services      │
│                        │  (Persistence)  │      (shared cache)            │
│                        │                 │                                 │
│                        └─────────────────┘                                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Features

- **BACnet/SC Support**: Secure Connect with TLS encryption and certificate authentication
- **KurrentDB Integration**: Persistent subscriptions with automatic catch-up
- **Redis Cache**: Persistent storage of all BACnet objects for crash recovery
- **BACnet CoV**: Change-of-Value notifications for real-time updates
- **Dynamic Objects**: Objects are fully defined from KurrentDB events
- **Multi-Subscription**: Multiple device subscriptions with instance offsets

## Prerequisites

- GCC/Clang Compiler
- CMake >= 3.16
- Redis Server >= 6.0
- KurrentDB (EventStoreDB) >= 23.x
- hiredis library
- cJSON library
- BACnet-Stack (optional, for full BACnet functionality)

### Ubuntu/Debian

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake pkg-config \
    libhiredis-dev libcjson-dev
```

## Quick Start with Docker

```bash
# Clone repository
git clone https://github.com/unlock-europe/bacnet-event-server.git
cd bacnet-event-server

# Customize configuration
cp config/server-config.json config/my-config.json
# Edit config/my-config.json

# Start
docker-compose up -d

# View logs
docker-compose logs -f bacnet-server

# With Redis Commander (Web UI)
docker-compose --profile dev up -d
# Open http://localhost:8081
```

## Manual Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

## Configuration

### Server Configuration (server-config.json)

```json
{
  "server": {
    "deviceInstance": 1234,
    "deviceName": "BACnet-Event-Server",
    "covLifetime": 300
  },
  "bacnetsc": {
    "enabled": true,
    "hubUri": "wss://bacnet-hub.example.com:443",
    "certificateFile": "/etc/bacnet-gateway/certs/device.pem",
    "privateKeyFile": "/etc/bacnet-gateway/certs/device-key.pem",
    "caCertificateFile": "/etc/bacnet-gateway/certs/ca.pem"
  },
  "kurrentdb": {
    "connectionString": "esdb://localhost:2113?tls=false"
  },
  "redis": {
    "host": "localhost",
    "port": 6379,
    "keyPrefix": "bacnet:"
  },
  "devices": [
    {
      "subscriptionId": "energy-meters",
      "streamName": "$ce-energy-meters",
      "groupName": "bacnet-gateway",
      "objectInstanceOffset": 0
    }
  ]
}
```

## KurrentDB Message Format

### ObjectDefinition

Defines a new BACnet object:

```json
{
  "messageType": "ObjectDefinition",
  "timestamp": "2024-12-14T10:30:00Z",
  "sourceId": "energy-meter-001",
  "payload": {
    "objectType": "analog-input",
    "objectInstance": 1,
    "objectName": "Total_Energy_kWh",
    "description": "Total energy consumption",
    "presentValueType": "real",
    "units": 169,
    "unitsText": "kWh",
    "covIncrement": 0.1,
    "initialValue": 0
  }
}
```

### ValueUpdate

Updates a value (triggers CoV if change > covIncrement):

```json
{
  "messageType": "ValueUpdate",
  "timestamp": "2024-12-14T10:30:15Z",
  "sourceId": "energy-meter-001",
  "payload": {
    "objectType": "analog-input",
    "objectInstance": 1,
    "presentValue": 15234.56,
    "quality": "good",
    "statusFlags": {
      "inAlarm": false,
      "fault": false,
      "overridden": false,
      "outOfService": false
    }
  }
}
```

### Supported Object Types

| Type | BACnet Object Type |
|------|-------------------|
| `analog-input` | AI (0) |
| `analog-output` | AO (1) |
| `analog-value` | AV (2) |
| `binary-input` | BI (3) |
| `binary-output` | BO (4) |
| `binary-value` | BV (5) |
| `multi-state-input` | MSI (13) |
| `multi-state-output` | MSO (14) |
| `multi-state-value` | MSV (19) |

## Redis Cache Structure

```
bacnet:object:<type>:<instance>  → JSON object definition
bacnet:objects:index             → SET of all object keys
bacnet:stream:positions          → HASH subscription → position
bacnet:device:config             → HASH device configuration
bacnet:events:value_change       → PUB/SUB channel for changes
```

## Spring Boot Integration

The Redis cache can be directly used by Spring Boot services:

```java
@Configuration
public class RedisConfig {
    @Bean
    public RedisTemplate<String, Object> redisTemplate(RedisConnectionFactory factory) {
        RedisTemplate<String, Object> template = new RedisTemplate<>();
        template.setConnectionFactory(factory);
        template.setKeySerializer(new StringRedisSerializer());
        template.setValueSerializer(new Jackson2JsonRedisSerializer<>(Object.class));
        return template;
    }
}

@Service
public class BacnetCacheService {
    @Autowired
    private RedisTemplate<String, Object> redisTemplate;
    
    public Optional<BacnetObject> getObject(int objectType, int objectInstance) {
        String key = String.format("bacnet:object:%d:%d", objectType, objectInstance);
        return Optional.ofNullable(redisTemplate.opsForValue().get(key))
            .map(obj -> objectMapper.convertValue(obj, BacnetObject.class));
    }
    
    // Subscribe to value changes
    @Bean
    public MessageListenerAdapter listenerAdapter() {
        return new MessageListenerAdapter(this, "onValueChange");
    }
    
    public void onValueChange(String message) {
        // message = "objectType:objectInstance"
        // Process value change...
    }
}
```

## Monitoring

### Status Endpoint (planned)

```bash
curl http://localhost:8080/status
```

### Redis Metrics

```bash
redis-cli INFO stats
redis-cli SCARD bacnet:objects:index  # Number of objects
```

## Error Handling

- **Redis Disconnect**: Automatic reconnect, objects remain in memory
- **KurrentDB Disconnect**: Reconnect with catch-up from last position
- **Server Restart**: Full recovery from Redis cache

## License

EUPL-1.2 License - Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy

## See Also

- [BACnet-Stack Repository](https://github.com/bacnet-stack/bacnet-stack)
- [KurrentDB Documentation](https://developers.eventstore.com/)
- [Redis Persistence](https://redis.io/docs/management/persistence/)
