#!/usr/bin/env python3
"""Interactive manual drive for one motor node.

Type an effort in [-1.0, 1.0] and press Enter. The current effort is re-sent
at 5 Hz, which keeps the Teensy's 1 s command watchdog from stopping the motor.
  0      -> stop (Teensy ramps down)
  q      -> send 0 and quit (Ctrl-C does the same)

Run monitor_state.py in a second terminal to watch position/velocity.

Usage:  python3 drive_motor.py            # motor 1
        python3 drive_motor.py --motor 2
"""

import argparse
import os
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'utils'))

from mqtt_handler import MQTTHandler

BROKER_IP = "192.168.10.1"
BROKER_PORT = 1883
PUBLISH_PERIOD_S = 0.2  # 5 Hz


def main():
    parser = argparse.ArgumentParser(description="Interactive manual motor drive")
    parser.add_argument("--motor", type=int, default=1, help="motor ID (1-4)")
    args = parser.parse_args()
    topic = f"robot/motor/{args.motor}/command"

    target = {"effort": 0.0}
    stop = threading.Event()
    mqtt_handler = MQTTHandler(BROKER_IP, BROKER_PORT)

    def publisher():
        while not stop.is_set():
            mqtt_handler.publish_effort(topic, target["effort"])
            stop.wait(PUBLISH_PERIOD_S)

    try:
        mqtt_handler.connect()
        threading.Thread(target=publisher, daemon=True).start()
        print(f"Driving motor {args.motor} on {topic}. Enter effort [-1, 1], 0 = stop, q = quit.\n")

        while True:
            line = input(f"effort [{target['effort']:+.2f}] > ").strip().lower()
            if line in ("q", "quit", "exit"):
                break
            if not line:
                continue
            try:
                effort = float(line)
            except ValueError:
                print("  ✗ Not a number")
                continue
            if not -1.0 <= effort <= 1.0:
                print("  ✗ Effort must be between -1.0 and 1.0")
                continue
            target["effort"] = effort

    except (KeyboardInterrupt, EOFError):
        print()
    finally:
        target["effort"] = 0.0
        stop.set()
        if mqtt_handler.connected:
            for _ in range(3):
                mqtt_handler.publish_effort(topic, 0.0)
                time.sleep(0.05)
            print("✓ Sent stop command (effort 0.0)")
        mqtt_handler.disconnect()


if __name__ == "__main__":
    main()
