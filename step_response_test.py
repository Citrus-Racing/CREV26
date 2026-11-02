#!/usr/bin/env python3

import sys
import time
import os
from datetime import datetime

# Add utils to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'utils'))

from mqtt_handler import MQTTHandler
from data_manager import DataManager, parse_motor_state

# Configuration
BROKER_IP = "192.168.10.1"
BROKER_PORT = 1883
TOPIC_COMMAND = "robot/motor/1/command"
TOPIC_STATE = "robot/motor/1/state"

# Global state
data_manager = DataManager()
start_time = None
on_duration = None
current_phase = "on"  # "on" or "off"
message_count = 0


def get_user_input() -> dict:
    """Prompt user for test parameters."""
    print("\n" + "="*60)
    print("STEP RESPONSE TEST - USER INPUT")
    print("="*60 + "\n")
    
    motor_id = int(input("Enter motor ID (1-4): "))
    
    while True:
        effort = float(input("Enter effort level (-1.0 to 1.0): "))
        if -1.0 <= effort <= 1.0:
            break
        print("  ✗ Effort must be between -1.0 and 1.0")
    
    while True:
        duration = float(input("Enter step duration (seconds): "))
        if duration > 0:
            break
        print("  ✗ Duration must be positive")
    
    notes = input("Enter optional notes (or press Enter): ").strip()
    
    return {
        "motor_id": motor_id,
        "effort": effort,
        "duration": duration,
        "notes": notes,
    }


def on_state_message(msg):
    """Callback when a state message arrives."""
    global start_time, on_duration, current_phase, message_count
    
    try:
        timestamp_us, effort_command, current, voltage, position, velocity = parse_motor_state(msg.payload)
        message_count += 1
        
        # Determine elapsed time and phase
        elapsed = time.time() - start_time
        if elapsed < on_duration:
            current_phase = "on"
            command_effort = params["effort"]
        else:
            current_phase = "off"
            command_effort = 0.0
        
        # Log the data (now includes effort_command from the Teensy)
        data_manager.log_state(timestamp_us, effort_command, current, voltage, position, velocity)
        
        # Print progress
        print(f"[{message_count:04d}] Phase: {current_phase:3s} | "
              f"Elapsed: {elapsed:6.2f}s | "
              f"Effort: {effort_command:6.3f} | "
              f"I={current:7.3f}A | "
              f"V={voltage:7.3f}V | "
              f"θ={position:7.3f}rad | "
              f"ω={velocity:7.3f}rad/s")
    
    except Exception as e:
        print(f"⚠ Error parsing message: {e}")

def publish_effort_loop(mqtt_handler, effort, duration):
    """Publish effort commands at 5 Hz for the specified duration (on-phase).
    Then publish 0.0 for the same duration (off-phase).
    """
    global start_time, on_duration
    
    start_time = time.time()
    on_duration = duration
    
    total_duration = duration * 2  # on + off
    publish_interval = 1.0 / 5  # 5 Hz publishing
    next_publish = time.time()
    
    print(f"\n{'='*60}")
    print(f"TEST RUNNING")
    print(f"{'='*60}\n")
    
    while time.time() - start_time < total_duration:
        elapsed = time.time() - start_time
        
        # Determine current phase and effort
        if elapsed < duration:
            command_effort = effort
            phase = "ON "
        else:
            command_effort = 0.0
            phase = "OFF"
        
        # Publish at 5 Hz
        if time.time() >= next_publish:
            mqtt_handler.publish_effort(TOPIC_COMMAND, command_effort)
            next_publish += publish_interval
        
        time.sleep(0.01)  # Small sleep to prevent busy-waiting
    
    print(f"\n✓ Test complete ({message_count} state messages received)")


def main():
    global params, on_duration
    
    # Get user input
    params = get_user_input()
    
    # Create MQTT handler
    mqtt_handler = MQTTHandler(BROKER_IP, BROKER_PORT)
    
    try:
        # Connect to broker
        mqtt_handler.connect()
        
        # Create CSV file
        filepath = data_manager.create_test_file(
            motor_id=params["motor_id"],
            effort=params["effort"],
            duration=params["duration"],
            notes=params["notes"],
            broker_ip=BROKER_IP
        )
        print(f"✓ Test file created: {filepath}\n")
        
        # Subscribe to state topic
        mqtt_handler.subscribe(TOPIC_STATE, on_state_message)
        
        # Run the test (publish commands and collect data)
        publish_effort_loop(mqtt_handler, params["effort"], params["duration"])
        
        # Write data to file
        data_manager.write_file()
        
    except KeyboardInterrupt:
        print("\n\n✗ Test interrupted by user")
    except Exception as e:
        print(f"\n✗ Error: {e}")
    finally:
        mqtt_handler.disconnect()
        print("✓ Shutdown complete\n")


if __name__ == "__main__":
    main()

