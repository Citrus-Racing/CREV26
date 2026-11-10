#!/usr/bin/env python3
"""Step test: command an effort for N seconds, then 0 for N seconds, recording at 1 kHz.

Note: the firmware ramp (RAMP_UP_PER_S / RAMP_DOWN_PER_S) still shapes the step.
"""

import os
import sys

# Add utils to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'utils'))

from test_runner import run_profile
from test_profiles import step_plan
from motors import MOTORS


def get_user_input() -> dict:
    """Prompt user for test parameters."""
    print("\n" + "=" * 60)
    print("STEP RESPONSE TEST - USER INPUT")
    print("=" * 60 + "\n")

    while True:
        motor_id = input(f"Enter motor ({', '.join(MOTORS)}): ").strip().upper()
        if motor_id in MOTORS:
            break
        print(f"  ✗ Motor must be one of {', '.join(MOTORS)}")

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

    return {"motor_id": motor_id, "effort": effort, "duration": duration, "notes": notes}


def main():
    p = get_user_input()
    try:
        plan = step_plan(p["effort"], p["duration"])
    except ValueError as e:
        print(f"✗ {e}")
        return
    print(f"\n{plan.description} ({plan.duration_s:g} s total)")
    for warning in plan.warnings:
        print(f"⚠ {warning}")

    run_profile(p["motor_id"], plan, notes=p["notes"])


if __name__ == "__main__":
    main()
