# CLAUDE.md

I am making a test bench for a new fsae EV division. the test bench has 4 motors that are currently free spinning, but we will later link pairs together to have them fight against each other to simulate load. The goal is to sent enough impulses to build accurate motor models. We have external encoders that are mounted right to each motor. The setup is each motor has its own mcu (a teensy) that is connected to this PC over ethernet which is the main node. The MCUs are responsible for reading the state of the motor (right now just the encoder but eventually the turn angle and power going into/out of the motor) as well as reading a command from the host PC to send a pwm signal to the motor controller.

If you ever have any questions about anything related to the setup just ask.

## Repo layout

- `firmware/motor_node/motor_node.ino`: Teensy 4.1 firmware, one copy per motor. Change `MOTOR_ID` to flash boards 2-4. Flashed by the user from the Arduino IDE; Claude can't compile or flash from its sandbox.
- `utils/mqtt_handler.py`, `utils/data_manager.py`: MQTT wrapper, CSV logging, and the state-packet parser.
- `monitor_state.py`: read-only live view of a motor's state topic.
- `drive_motor.py`: interactive manual effort commands.
- `step_response_test.py`: on/off effort test that logs to `data/motor_<id>/*.csv`. Run it from the repo root, since `data/` is a relative path.
- `data/`: recorded runs. Files from before 2026-11-02 used older firmware, where position/velocity were placeholders.

## MQTT interface

- Broker: `192.168.10.1:1883` (this PC). Teensys get their IP via DHCP.
- `robot/motor/<id>/command`: one little-endian float32 effort in [-1, 1].
- `robot/motor/<id>/state`: 24-byte packed struct at 5 Hz, `struct.unpack('<Ifffif')`:
  `micros, effort_out, current, voltage, encoder_count (int32), velocity_rad_s`.
  - Position is sent as raw counts because a float in radians loses resolution after a few thousand revolutions. `parse_motor_state()` converts it to `position_rad`.
  - Current and voltage are not wired up or calibrated yet.
  - The firmware and `utils/data_manager.py` must match: after changing the packet, reflash and update the parser together.

## Bench hardware notes

- **Motors**: 3 kW BLDC e-scooter kit motors running on ~72-80 VDC. The user's revised max-speed estimate is ~4900 rpm.
- **Controller**: an unknown cheap e-scooter kit controller with 3.3 V logic.
  - It takes a PWM "throttle" signal (pin 23, 100 kHz) and a direction input that's open/closed rather than high/low (pin 22, `OUTPUT_OPENDRAIN`): released = forward, pulled to GND = reverse.
  - **It won't start the motor from a hard PWM step.** Effort must be ramped (0.25 effort/s is known to work). An RC filter on a stepped PWM didn't help.
  - **Measured behavior** (2026-11-02, motor 1 free-spinning, runs `data/motor_1/2026-11-02T18-*`):
    - The controller treats the PWM as an analog throttle voltage (duty × 3.3 V) with a dead band.
    - The motor starts at |effort| ≈ 0.42 (about 1.4 V) and stops below about 0.36 (about 1.2 V).
    - Above the dead band, steady forward speed ≈ 5990 rpm × (effort − 0.37): 775 rpm at 0.5 and 3769 rpm at 1.0.
    - Top speed (3769 rpm) is likely capped by the 3.3 V PWM. Scooter throttles normally reach full scale at about 4-5 V, and the motor should do about 4900 rpm.
    - Reverse at −0.5 gave −464 rpm, about 60% of forward. The user confirms reverse is factory speed-limited; it's untested at −1.0.
    - Speed tracks the throttle closely on the way down too, so the controller appears to brake actively rather than let the motor coast.
- **Encoder**: Taiss 600 P/R optical encoder (LPD3806-600BM style), NPN open collector.
  - A goes to pin 18 and B to pin 17, each with roughly a 1 kΩ pull-up to the Teensy's 3.3 V.
  - **Power it from its own 9–24 V supply, with the supply's negative tied to Teensy GND.** Do NOT power it from the Teensy's VIN pin: on USB power VIN measured 4.3 V, below the encoder's 5 V minimum, and both outputs sat stuck low (found 2026-11-02). A 9 V battery is the temporary fix; a permanent supply is TBD.
  - Because the outputs are open collector, a higher encoder supply voltage never reaches the Teensy pins. The pull-ups set the high level, so they must stay on 3.3 V.
  - **The two pull-up resistors must not touch each other.** On motor 1 their leads were shorting, which tied A to B. The decoder then counted +2 on every edge, so velocity was always positive in both directions (found 2026-11-02).
  - Direction is set in software with `ENCODER_SIGN` in the firmware, so positive effort reads as positive velocity. Motor 1 = -1. Check it on each new board.
  - With 4x decoding that's 2400 counts/rev.
  - This unit is rated for 6400 rpm (per the user's spec; generic LPD3806 listings quote lower figures, so ignore those).
  - It's mounted directly on the motor shaft.
- **Safety**: a physical killswitch on the HV lines kills everything, and the spinning parts are covered.

## Working rules

- **The user runs anything that commands a motor to move.** Claude gives the exact commands to run, and must not publish to `robot/motor/+/command` until the user explicitly hands over that control.
- Claude may passively subscribe to state topics. That's read-only and sends no commands.
