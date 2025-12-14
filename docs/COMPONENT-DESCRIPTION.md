# BACnet Event Server

## Component Description

The **BACnet Event Server** is a gateway that makes measurement data from event-driven systems available via the BACnet/SC (Secure Connect) protocol. It acts as a bridge between modern event streaming architectures and building automation systems.

```
┌─────────────────┐      ┌─────────────────────┐      ┌─────────────────────┐
│                 │      │                     │      │                     │
│   KurrentDB     │─────▶│  BACnet Event       │─────▶│  BACnet/SC Clients  │
│   (Events)      │      │  Server             │      │  (BMS, SCADA, DDC)  │
│                 │      │                     │      │                     │
└─────────────────┘      └──────────┬──────────┘      └─────────────────────┘
                                    │
                                    ▼
                         ┌─────────────────────┐
                         │    Redis Cache      │
                         │    (Recovery)       │
                         └─────────────────────┘
```

---

## What is KurrentDB?

**KurrentDB** (formerly EventStoreDB) is an event streaming database optimized for Event Sourcing and CQRS architectures. Unlike traditional databases that store the current state, KurrentDB stores every change as an immutable event in an append-only log.

**Key Features:**
- **Event Streams:** Data is organized in streams (e.g., `energy-meter-001`, `temperature-sensor-042`)
- **Immutable History:** Every event is preserved - ideal for audit trails and analytics
- **Subscriptions:** Clients can subscribe to streams and receive events in real-time
- **Projections:** Built-in capability to transform and aggregate events

**Example Event:**
```json
{
  "eventType": "MeasurementRecorded",
  "data": {
    "sensorId": "temp-sensor-01",
    "value": 21.5,
    "unit": "°C",
    "timestamp": "2024-12-14T10:30:00Z"
  }
}
```

---

## Why Persistent Subscriptions?

A common question: *If only the latest values are published via BACnet, why use Persistent Subscriptions instead of simple Catch-up Subscriptions?*

### The Challenge

The BACnet Event Server may be offline due to:
- Planned maintenance or updates
- Container restarts in Kubernetes/Docker environments
- Network interruptions
- System failures

During downtime, events continue to arrive in KurrentDB. Without Persistent Subscriptions, these events would be lost.

### The Solution: Persistent Subscriptions

```
Timeline:
─────────────────────────────────────────────────────────────────▶

KurrentDB:     E1    E2    E3    E4    E5    E6    E7    E8    E9
               │     │     │     │     │     │     │     │     │
               ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼     ▼
Server:      [Online]    [OFFLINE - Restart]    [Online - Catch-up]
               │     │                           │     │     │
Processed:    E1    E2                          E3    E4    E5...
                                                 ▲
                                                 │
                                    Persistent Subscription
                                    remembers position!
```

**Persistent Subscriptions provide:**

1. **Server-side Position Tracking**
   - KurrentDB remembers which events have been acknowledged
   - After restart, processing continues exactly where it stopped

2. **Guaranteed Delivery**
   - Every event is processed at least once
   - No measurement values are lost during downtime

3. **Automatic Catch-up**
   - After restart, all missed events are delivered in order
   - The server quickly returns to the current state

4. **Consumer Groups**
   - Multiple server instances can share the workload
   - Enables horizontal scaling and high availability

### Why This Matters for BACnet

Even though BACnet only shows the *current* value, Persistent Subscriptions ensure:

| Scenario | Without Persistent Sub | With Persistent Sub |
|----------|----------------------|---------------------|
| Server restart during value change | Old value remains | New value is applied |
| Object definition during downtime | Object missing | Object is created |
| Alarm event during maintenance | Alarm lost | Alarm is processed |
| Status change (online→fault) | Status incorrect | Status updated |

**The critical insight:** BACnet displays the *result* of all events - missing even one event can leave the system in an inconsistent state.

---

## BACnet/SC (Secure Connect)

The BACnet Event Server uses **BACnet/SC** - the modern, secure transport layer for BACnet:

**Advantages over BACnet/IP:**
- **TLS Encryption:** All communication is encrypted
- **Certificate-based Authentication:** Devices authenticate via X.509 certificates
- **WebSocket Transport:** Firewall-friendly, works over standard HTTPS ports
- **Hub-and-Spoke Topology:** Simplified network architecture

**Typical Deployment:**
```
                    ┌──────────────────┐
                    │   BACnet/SC Hub  │
                    │   (Primary)      │
                    └────────┬─────────┘
                             │ TLS/WebSocket
           ┌─────────────────┼─────────────────┐
           │                 │                 │
           ▼                 ▼                 ▼
    ┌─────────────┐   ┌─────────────┐   ┌─────────────┐
    │ BACnet      │   │ Building    │   │ Energy      │
    │ Event Server│   │ Controller  │   │ Management  │
    └─────────────┘   └─────────────┘   └─────────────┘
```

---

## Use Cases

### 1. Energy Monitoring
Publish energy meter readings from IoT sensors to building management systems:
- Total consumption (kWh)
- Current power (kW)
- Power factor, voltage, current

### 2. Environmental Monitoring
Expose environmental sensor data:
- Temperature, humidity
- CO₂ levels, air quality
- Occupancy status

### 3. Equipment Status
Bridge industrial equipment status to BACnet:
- Operating states (on/off/fault)
- Runtime counters
- Alarm conditions

### 4. Renewable Energy Integration
Publish data from solar/battery systems:
- PV generation
- Battery state of charge
- Grid feed-in/consumption

---

## Message Types

The server processes these KurrentDB event types:

| Message Type | Purpose |
|-------------|---------|
| `ObjectDefinition` | Create/update a BACnet object with metadata |
| `ValueUpdate` | Update the present value (triggers CoV) |
| `ObjectDelete` | Remove a BACnet object |
| `DeviceConfig` | Configure device-level properties |

---

## Supported BACnet Object Types

| Type | Description | Typical Use |
|------|-------------|-------------|
| Analog Input (AI) | Read-only analog values | Sensor readings |
| Analog Output (AO) | Writable analog values | Setpoints |
| Analog Value (AV) | General analog values | Calculated values |
| Binary Input (BI) | Read-only binary states | Contact status |
| Binary Output (BO) | Writable binary states | Relay control |
| Binary Value (BV) | General binary values | Mode flags |
| Multi-State Input (MSI) | Enumerated states | Equipment status |
| Multi-State Output (MSO) | Writable states | Mode selection |
| Multi-State Value (MSV) | General enumerated | Operating modes |

---

## Requirements

- **KurrentDB** 23.x or later
- **Redis** 6.0 or later (for persistence)
- **Network:** Outbound HTTPS for BACnet/SC hub connection
- **Certificates:** X.509 certificates for BACnet/SC authentication

---

## License

EUPL-1.2 - Copyright (c) 2024 Unlock Europe - Free and Open Source Software - Energy
