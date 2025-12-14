#!/usr/bin/env python3
"""
BACnet Event Server - Load Generator

Generates test events for KurrentDB to stress test the BACnet Event Server.

Usage:
    python load_generator.py [rate] [duration_seconds]
    
    rate: messages per second (default: 100)
    duration_seconds: how long to run (default: indefinite)

Example:
    python load_generator.py 100 3600  # 100 msg/s for 1 hour
"""

import asyncio
import json
import random
import time
import sys
from datetime import datetime, timezone
from typing import Dict, List, Tuple, Optional

try:
    from esdbclient import EventStoreDBClient, NewEvent
except ImportError:
    print("Installing esdbclient...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "esdbclient"])
    from esdbclient import EventStoreDBClient, NewEvent

# Object type distribution
OBJECT_TYPES: List[Tuple[str, float]] = [
    ("analog-input", 0.40),
    ("analog-value", 0.30),
    ("binary-input", 0.15),
    ("binary-value", 0.10),
    ("multi-state-value", 0.05)
]

# Message type distribution
MESSAGE_TYPES: List[Tuple[str, float]] = [
    ("ValueUpdate", 0.90),
    ("ObjectDefinition", 0.08),
    ("ObjectDelete", 0.02)
]

# BACnet units (common examples)
UNITS = {
    "analog-input": [(169, "kWh"), (95, ""), (62, "degC"), (91, "percent")],
    "analog-value": [(95, ""), (169, "kWh"), (119, "kW")],
    "binary-input": [(95, "")],
    "binary-value": [(95, "")],
    "multi-state-value": [(95, "")]
}


class LoadGenerator:
    """Generates test load for KurrentDB"""
    
    def __init__(self, connection_string: str = "esdb://localhost:2113?tls=false",
                 target_rate: int = 100):
        self.connection_string = connection_string
        self.target_rate = target_rate
        self.client: Optional[EventStoreDBClient] = None
        self.objects: Dict[Tuple[str, int], dict] = {}
        self.next_instance: Dict[str, int] = {}
        self.stats = {
            "sent": 0,
            "errors": 0,
            "start_time": time.time(),
            "by_type": {
                "ObjectDefinition": 0,
                "ValueUpdate": 0,
                "ObjectDelete": 0
            }
        }
    
    def connect(self) -> bool:
        """Connect to KurrentDB"""
        try:
            self.client = EventStoreDBClient(uri=self.connection_string)
            print(f"[LOADGEN] Connected to {self.connection_string}")
            return True
        except Exception as e:
            print(f"[LOADGEN] Connection failed: {e}")
            return False
    
    def weighted_choice(self, choices: List[Tuple[str, float]]) -> str:
        """Select item based on weights"""
        r = random.random()
        cumulative = 0.0
        for choice, weight in choices:
            cumulative += weight
            if r <= cumulative:
                return choice
        return choices[-1][0]
    
    def get_next_instance(self, obj_type: str) -> int:
        """Get next instance number for object type"""
        if obj_type not in self.next_instance:
            self.next_instance[obj_type] = 1
        instance = self.next_instance[obj_type]
        self.next_instance[obj_type] += 1
        return instance
    
    def generate_object_definition(self, obj_type: str, instance: int) -> dict:
        """Generate ObjectDefinition message"""
        units = random.choice(UNITS.get(obj_type, [(95, "")]))
        
        if obj_type.startswith("analog"):
            value_type = "real"
            initial_value = random.uniform(0, 100)
            cov_increment = random.choice([0.1, 0.5, 1.0])
        elif obj_type.startswith("binary"):
            value_type = "boolean"
            initial_value = random.choice([True, False])
            cov_increment = None
        else:  # multi-state
            value_type = "unsigned"
            initial_value = random.randint(1, 5)
            cov_increment = None
        
        msg = {
            "messageType": "ObjectDefinition",
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "sourceId": "load-generator",
            "payload": {
                "objectType": obj_type,
                "objectInstance": instance,
                "objectName": f"Test_{obj_type.replace('-', '_')}_{instance}",
                "description": f"Load test object {instance}",
                "presentValueType": value_type,
                "units": units[0],
                "unitsText": units[1],
                "initialValue": initial_value
            }
        }
        
        if cov_increment:
            msg["payload"]["covIncrement"] = cov_increment
        
        # Store object for later updates
        self.objects[(obj_type, instance)] = {
            "type": obj_type,
            "instance": instance,
            "value_type": value_type,
            "last_value": initial_value
        }
        
        return msg
    
    def generate_value_update(self, obj_type: str, instance: int) -> dict:
        """Generate ValueUpdate message"""
        obj = self.objects.get((obj_type, instance))
        if not obj:
            return self.generate_object_definition(obj_type, instance)
        
        # Generate new value based on type
        if obj["value_type"] == "real":
            # Random walk from last value
            delta = random.gauss(0, 1.0)
            new_value = max(0, min(100, obj["last_value"] + delta))
        elif obj["value_type"] == "boolean":
            # 10% chance to flip
            new_value = not obj["last_value"] if random.random() < 0.1 else obj["last_value"]
        else:  # unsigned (multi-state)
            # Random state 1-5
            new_value = random.randint(1, 5)
        
        obj["last_value"] = new_value
        
        # Occasionally set status flags
        in_alarm = random.random() < 0.02
        fault = random.random() < 0.01
        
        return {
            "messageType": "ValueUpdate",
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "sourceId": "load-generator",
            "payload": {
                "objectType": obj_type,
                "objectInstance": instance,
                "presentValue": new_value,
                "quality": "good" if not (in_alarm or fault) else "uncertain",
                "statusFlags": {
                    "inAlarm": in_alarm,
                    "fault": fault,
                    "overridden": False,
                    "outOfService": False
                }
            }
        }
    
    def generate_object_delete(self, obj_type: str, instance: int) -> dict:
        """Generate ObjectDelete message"""
        # Remove from tracked objects
        key = (obj_type, instance)
        if key in self.objects:
            del self.objects[key]
        
        return {
            "messageType": "ObjectDelete",
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "sourceId": "load-generator",
            "payload": {
                "objectType": obj_type,
                "objectInstance": instance,
                "reason": "load-test-cleanup"
            }
        }
    
    def generate_message(self) -> dict:
        """Generate a random message based on distribution"""
        msg_type = self.weighted_choice(MESSAGE_TYPES)
        obj_type = self.weighted_choice(OBJECT_TYPES)
        
        if msg_type == "ObjectDefinition":
            instance = self.get_next_instance(obj_type)
            return self.generate_object_definition(obj_type, instance)
        
        elif msg_type == "ValueUpdate":
            if not self.objects:
                # Create first object if none exist
                instance = self.get_next_instance(obj_type)
                return self.generate_object_definition(obj_type, instance)
            
            # Update random existing object
            obj_key = random.choice(list(self.objects.keys()))
            return self.generate_value_update(obj_key[0], obj_key[1])
        
        else:  # ObjectDelete
            if len(self.objects) > 20:  # Keep minimum objects
                obj_key = random.choice(list(self.objects.keys()))
                return self.generate_object_delete(obj_key[0], obj_key[1])
            else:
                # Not enough objects, send value update instead
                if self.objects:
                    obj_key = random.choice(list(self.objects.keys()))
                    return self.generate_value_update(obj_key[0], obj_key[1])
                else:
                    instance = self.get_next_instance(obj_type)
                    return self.generate_object_definition(obj_type, instance)
    
    async def send_message(self, message: dict) -> bool:
        """Send message to KurrentDB"""
        try:
            event_data = json.dumps(message).encode('utf-8')
            event = NewEvent(
                type=message["messageType"],
                data=event_data
            )
            
            self.client.append_to_stream(
                stream_name="energy-meters",
                events=[event],
                current_version="any"
            )
            
            self.stats["sent"] += 1
            self.stats["by_type"][message["messageType"]] += 1
            return True
            
        except Exception as e:
            self.stats["errors"] += 1
            print(f"[LOADGEN] Error: {e}")
            return False
    
    def print_stats(self):
        """Print current statistics"""
        elapsed = time.time() - self.stats["start_time"]
        rate = self.stats["sent"] / elapsed if elapsed > 0 else 0
        
        print(f"[LOADGEN] Sent: {self.stats['sent']:,} | "
              f"Rate: {rate:.1f}/s | "
              f"Objects: {len(self.objects)} | "
              f"Errors: {self.stats['errors']} | "
              f"Elapsed: {elapsed:.0f}s")
        print(f"          Types: ObjDef={self.stats['by_type']['ObjectDefinition']}, "
              f"ValUpd={self.stats['by_type']['ValueUpdate']}, "
              f"ObjDel={self.stats['by_type']['ObjectDelete']}")
    
    async def run(self, duration_seconds: Optional[int] = None):
        """Run the load generator"""
        if not self.connect():
            return
        
        interval = 1.0 / self.target_rate
        end_time = time.time() + duration_seconds if duration_seconds else None
        stats_interval = self.target_rate * 10  # Print stats every ~10 seconds
        
        print(f"[LOADGEN] Starting at {self.target_rate} msg/s")
        if duration_seconds:
            print(f"[LOADGEN] Duration: {duration_seconds} seconds")
        print()
        
        try:
            while True:
                if end_time and time.time() >= end_time:
                    break
                
                start = time.time()
                
                message = self.generate_message()
                await self.send_message(message)
                
                # Print stats periodically
                if self.stats["sent"] % stats_interval == 0:
                    self.print_stats()
                
                # Pace to target rate
                elapsed = time.time() - start
                if elapsed < interval:
                    await asyncio.sleep(interval - elapsed)
                    
        except KeyboardInterrupt:
            print("\n[LOADGEN] Interrupted by user")
        
        print()
        print("[LOADGEN] Final Statistics:")
        self.print_stats()


async def main():
    """Main entry point"""
    rate = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else None
    
    connection_string = "esdb://kurrentdb:2113?tls=false"
    
    generator = LoadGenerator(connection_string, rate)
    await generator.run(duration)


if __name__ == "__main__":
    asyncio.run(main())
