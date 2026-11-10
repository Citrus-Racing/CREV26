#!/usr/bin/env python3
"""Live, read-only view of a motor node's state. Sends NO commands.

Usage:  python3 monitor_state.py            # LF
        python3 monitor_state.py --motor RR
Ctrl-C to quit.
"""

import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'utils'))

from mqtt_handler import MQTTHandler
from data_manager import parse_state_packet, ENCODER_COUNTS_PER_REV
from motors import add_motor_arg, state_topic
from sensors import current_sensor

BROKER_IP = "192.168.10.1"
BROKER_PORT = 1883
PRINT_EVERY_N_PACKETS = 4   # packets arrive at 20 Hz -> print at 5 Hz


def main():
    parser = argparse.ArgumentParser(description="Read-only live motor state viewer")
    add_motor_arg(parser)
    args = parser.parse_args()
    topic = state_topic(args.motor)
    cs = current_sensor(args.motor)

    stats = {"n": 0, "last_seq": None, "lost": 0}

    def on_state(msg):
        try:
            pkt = parse_state_packet(msg.payload)
        except ValueError as e:
            print(f"⚠ {e}")
            return
        if stats["last_seq"] is not None:
            stats["lost"] += max(0, pkt.sequence - stats["last_seq"] - 1)
        stats["last_seq"] = pkt.sequence
        stats["n"] += 1
        if stats["n"] % PRINT_EVERY_N_PACKETS:
            return

        ts_us, count, effort = pkt.samples[-1]
        rpm = pkt.velocity_rad_s() * 60.0 / (2.0 * math.pi)
        amps = cs.amps(pkt.current_sensor_v) if cs else None
        current = (f" | I={amps:+6.2f} A ({pkt.current_sensor_v:.3f} V)"
                   if amps is not None else "")
        print(f"t={ts_us / 1e6:10.3f}s | effort={effort:+.3f} | "
              f"count={count:+11d} ({count / ENCODER_COUNTS_PER_REV:+9.2f} rev) | "
              f"{rpm:+7.0f} rpm{current} | lost pkts={stats['lost']} dropped={pkt.dropped_samples}")

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
