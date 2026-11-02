#!/usr/bin/env python3
"""Slow sweep test: ramp effort 0 -> max -> 0 (forward, then reverse), recording at 1 kHz.

One run maps effort -> steady speed across the whole range, including the
controller's dead band, its start/stop hysteresis, and the reverse limit.
(The same test can be started from the control panel.)

Usage:
  python3 sweep_test.py                         # motor 1, both directions, 0->1->0 over 30 s each way
  python3 sweep_test.py --direction forward --ramp-time 60
  python3 sweep_test.py --motor 2 --max-effort 0.8
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), 'utils'))

from test_runner import run_profile
from test_profiles import sweep_plan


def main():
    parser = argparse.ArgumentParser(description="Slow effort sweep with 1 kHz logging")
    parser.add_argument("--motor", type=int, default=1, help="motor ID (1-4)")
    parser.add_argument("--max-effort", type=float, default=1.0, help="peak |effort| (0-1]")
    parser.add_argument("--ramp-time", type=float, default=30.0, help="seconds from 0 to peak")
    parser.add_argument("--hold", type=float, default=2.0, help="seconds held at peak")
    parser.add_argument("--direction", choices=["both", "forward", "reverse"], default="both")
    parser.add_argument("--notes", default="")
    args = parser.parse_args()

    try:
        plan = sweep_plan(args.max_effort, args.ramp_time, args.hold, args.direction)
    except ValueError as e:
        parser.error(str(e))

    print(f"\nMotor {args.motor}: {plan.description} ({plan.duration_s:.0f} s total)")
    for warning in plan.warnings:
        print(f"⚠ {warning}")
    try:
        input("Press Enter to start (Ctrl-C to cancel)... ")
    except (KeyboardInterrupt, EOFError):
        print("\nCancelled")
        return

    run_profile(args.motor, plan, notes=args.notes)


if __name__ == "__main__":
    main()
