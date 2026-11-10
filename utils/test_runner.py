"""Shared machinery for scripted motor tests (terminal scripts).

run_profile() runs a TestPlan (see test_profiles.py) on one motor node:
it publishes the effort-vs-time profile while recording every 1 kHz state
sample to a CSV. It always finishes by commanding 0, including after Ctrl-C
or an error. The control panel runs the same TestPlans itself.
"""

import math
import threading
import time

from mqtt_handler import MQTTHandler
from data_manager import DataManager, parse_state_packet
from test_profiles import TestPlan
from motors import command_topic, state_topic

BROKER_IP = "192.168.10.1"
BROKER_PORT = 1883
COMMAND_RATE_HZ = 20     # well inside the firmware's 1 s command watchdog
PRINT_RATE_HZ = 2


def run_profile(motor_id: str, plan: TestPlan, notes: str = "") -> None:
    """Run plan.profile(t) for t in [0, plan.duration_s] and record the motor's state."""
    profile, duration_s = plan.profile, plan.duration_s
    topic_cmd = command_topic(motor_id)
    topic_state = state_topic(motor_id)

    mqtt_handler = MQTTHandler(BROKER_IP, BROKER_PORT)
    data_manager = DataManager()
    latest = {"packet": None, "errors": 0}
    recording = threading.Event()

    def on_state(msg):
        try:
            packet = parse_state_packet(msg.payload)
        except ValueError as e:
            if latest["errors"] == 0:
                print(f"⚠ {e}")
            latest["errors"] += 1
            return
        latest["packet"] = packet
        if recording.is_set():
            data_manager.log_packet(packet)

    try:
        mqtt_handler.connect()
        mqtt_handler.subscribe(topic_state, on_state)

        # Make sure the node is alive and speaking the current packet format
        time.sleep(1.0)
        if latest["packet"] is None:
            raise RuntimeError(f"No valid state packets from motor {motor_id} "
                               f"(got {latest['errors']} bad ones). Is the Teensy running "
                               f"the current firmware?")

        path = data_manager.create_test_file(motor_id, plan.test_type, plan.tag,
                                             {**plan.params, "source": "script"}, notes, BROKER_IP)
        print(f"✓ Recording to {path}\n")
        recording.set()

        t0 = time.monotonic()
        next_cmd = t0
        next_print = t0
        while True:
            now = time.monotonic()
            t = now - t0
            if t >= duration_s:
                break
            effort = profile(t)
            mqtt_handler.publish_effort(topic_cmd, effort)

            if now >= next_print:
                packet = latest["packet"]
                rpm = packet.velocity_rad_s() * 60.0 / (2.0 * math.pi)
                print(f"t={t:6.1f}/{duration_s:.0f}s | cmd={effort:+.3f} | "
                      f"out={packet.samples[-1][2]:+.3f} | {rpm:+7.0f} rpm")
                next_print += 1.0 / PRINT_RATE_HZ

            next_cmd += 1.0 / COMMAND_RATE_HZ
            time.sleep(max(0.0, next_cmd - time.monotonic()))

        print("\n✓ Profile complete")

    except KeyboardInterrupt:
        print("\n✗ Interrupted by user")
    except Exception as e:
        print(f"\n✗ Error: {e}")
    finally:
        # Always tell the motor to stop, even after Ctrl-C or an error
        if mqtt_handler.connected:
            for _ in range(3):
                mqtt_handler.publish_effort(topic_cmd, 0.0)
                time.sleep(0.05)
            print("✓ Sent stop command (effort 0.0)")
            time.sleep(0.3)   # collect packets still in flight
        recording.clear()
        if data_manager.has_data:
            data_manager.write_file()
        mqtt_handler.disconnect()
