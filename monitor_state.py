#!/usr/bin/env python3
"""Live, read-only view of a motor node's state. Sends NO commands.

Usage:  python3 monitor_state.py            # motor 1
        python3 monitor_state.py --motor 2
Ctrl-C to quit.
"""

import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'utils'))

from mqtt_handler import MQTTHandler
from data_manager import parse_motor_state

BROKER_IP = "192.168.10.1"
BROKER_PORT = 1883


def main():
    parser = argparse.ArgumentParser(description="Read-only live motor state viewer")
    parser.add_argument("--motor", type=int, default=1, help="motor ID (1-4)")
    args = parser.parse_args()
    topic = f"robot/motor/{args.motor}/state"

    def on_state(msg):
        try:
            ts_us, effort, _current, _voltage, count, pos, vel = parse_motor_state(msg.payload)
        except Exception as e:
            print(f"⚠ Bad packet ({len(msg.payload)} bytes): {e}")
            return
        rpm = vel * 60.0 / (2.0 * math.pi)
        print(f"t={ts_us / 1e6:10.3f}s | effort={effort:+.3f} | "
              f"count={count:+11d} ({pos / (2 * math.pi):+9.2f} rev) | "
              f"vel={vel:+8.2f} rad/s ({rpm:+6.0f} rpm)")

    mqtt_handler = MQTTHandler(BROKER_IP, BROKER_PORT)
    try:
        mqtt_handler.connect()
        mqtt_handler.subscribe(topic, on_state)
        print("Listening (read-only). Ctrl-C to quit.\n")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print()
    finally:
        mqtt_handler.disconnect()


if __name__ == "__main__":
    main()
