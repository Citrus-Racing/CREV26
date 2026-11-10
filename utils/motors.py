"""Motor names and MQTT topics. Each motor node is named after its wheel corner.

Must match MOTOR_NAME in the firmware (firmware/make_corner_sketches.py).
"""

MOTORS = ("LF", "RF", "LR", "RR")   # left/right, front/rear


def state_topic(motor: str) -> str:
    return f"robot/motor/{motor}/state"


def command_topic(motor: str) -> str:
    return f"robot/motor/{motor}/command"


def add_motor_arg(parser, default: str = "LF"):
    """Add --motor to an argparse parser; accepts any case (lf -> LF)."""
    parser.add_argument("--motor", type=str.upper, choices=MOTORS, default=default,
                        help=f"wheel corner: {', '.join(MOTORS)} (default {default})")
