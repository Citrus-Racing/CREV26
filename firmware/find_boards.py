#!/usr/bin/env python3
"""Find each corner's Teensy by its factory USB serial number. Read-only.

    python3 firmware/find_boards.py

/dev/ttyACM* numbers and USB port addresses change when boards are unplugged or
replugged. Each Teensy's serial number doesn't. BOARD_SERIALS in
make_corner_sketches.py says which Teensy belongs to which corner. This script
looks up where each one is plugged in right now, and checks that it's running
its own corner's firmware (the firmware sets the USB product name to
"Motor_<corner>").

For each corner it prints the serial device and the arduino-cli upload port:
    arduino-cli compile --fqbn teensy:avr:teensy41 --upload -p <upload port> firmware/motor_node_<corner>

Exits with status 1 if a corner's board is missing, a board is running some
other firmware, or a Teensy that isn't in BOARD_SERIALS is plugged in.
Only reads /sys; it never opens a port or talks to a board.
"""

import glob
import os
import sys

sys.dont_write_bytecode = True  # keep __pycache__ out of the firmware folder
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_corner_sketches import BOARD_SERIALS  # noqa: E402

TEENSY_VENDOR_ID = "16c0"  # PJRC
SYS_USB_DEVICES = "/sys/bus/usb/devices"


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().strip()
    except OSError:
        return None


def connected_teensys():
    boards = []
    for entry in sorted(glob.glob(os.path.join(SYS_USB_DEVICES, "*"))):
        if read(os.path.join(entry, "idVendor")) != TEENSY_VENDOR_ID:
            continue  # interfaces, hubs and other USB devices
        dev = os.path.realpath(entry)
        parts = dev.split("/")
        # arduino-cli's Teensy port address is the sysfs path from the root hub down,
        # e.g. usb1/1-1/1-1.4/1-1.4.4
        root = next((i for i, p in enumerate(parts) if p.startswith("usb")), None)
        ttys = sorted(glob.glob(os.path.join(dev, "*:*", "tty", "tty*")))
        boards.append({
            "serial": read(os.path.join(dev, "serial")),
            "product": read(os.path.join(dev, "product")),
            "upload_port": "/".join(parts[root:]) if root is not None else "?",
            "tty": "/dev/" + os.path.basename(ttys[0]) if ttys else None,
        })
    return boards


def main():
    boards = connected_teensys()
    by_serial = {b["serial"]: b for b in boards}
    problems = []

    print(f"{'corner':<7}{'serial':<10}{'serial device':<15}{'upload port (-p)':<26}firmware")
    for corner, serial in BOARD_SERIALS.items():
        b = by_serial.get(serial)
        if b is None:
            print(f"{corner:<7}{serial:<10}NOT CONNECTED")
            problems.append(f"{corner}: no Teensy with serial {serial} is plugged in")
            continue
        expected = f"Motor_{corner}"
        running = b["product"] or "?"
        status = "ok" if running == expected else f"MISMATCH (expected {expected})"
        if running != expected:
            problems.append(f"{corner}: board {serial} is running {running!r}, not {expected!r}")
        print(f"{corner:<7}{serial:<10}{b['tty'] or 'none':<15}{b['upload_port']:<26}{running}  {status}")

    for b in boards:
        if b["serial"] not in BOARD_SERIALS.values():
            problems.append(f"unknown Teensy (serial {b['serial']}, running {b['product']!r}) "
                            f"at {b['upload_port']}: not in BOARD_SERIALS")

    print()
    if problems:
        for p in problems:
            print("✗", p)
        print("Fix the wiring or BOARD_SERIALS in firmware/make_corner_sketches.py before flashing.")
        sys.exit(1)
    print("✓ all corners found, each running its own corner's firmware")


if __name__ == "__main__":
    main()
