# CLAUDE.md

I am making a test bench for a new fsae EV division. the test bench has 4 motors that are currently free spinning, but we will later link pairs together to have them fight against each other to simulate load. The goal is to sent enough impulses to build accurate motor models. We have external encoders that are mounted right to each motor. The setup is each motor has its own mcu (a teensy) that is connected to this PC over ethernet which is the main node. The MCUs are responsible for reading the state of the motor (right now just the encoder but eventually the turn angle and power going into/out of the motor) as well as reading a command from the host PC to send a pwm signal to the motor controller.

If you ever have any questions about anything related to the setup just ask.

## Repo layout

- `firmware/motor_node/motor_node.ino`: Teensy 4.1 firmware, one copy per motor. Change `MOTOR_ID` (and check `ENCODER_SIGN`) to flash boards 2-4. Flashed by the user from the Arduino IDE; Claude can't compile or flash from its sandbox.
  - A 1 kHz `IntervalTimer` interrupt does the control update (watchdog, ramp, PWM) and the encoder sample into a ring buffer. `loop()` only handles networking.
- `utils/data_manager.py`: state-packet parser plus CSV logging; counts lost packets and Teensy-dropped samples.
- `utils/test_profiles.py`: `step_plan()` and `sweep_plan()` build a validated `TestPlan` (profile, duration, CSV name and metadata, warnings). The scripts and the control panel both use these, so a test produces the same CSV either way. Add new test types here.
- `utils/test_runner.py`: `run_profile(motor_id, plan, notes)` for the terminal scripts. It sends effort(t) at 20 Hz, records at 1 kHz, refuses to start if packets don't parse, and always sends 0 at the end.
- `utils/mqtt_handler.py`: paho wrapper.
- `monitor_state.py`: read-only live view.
- `drive_motor.py`: interactive manual effort commands.
- `step_response_test.py`: on/off effort test (prompts for parameters).
- `sweep_test.py`: slow 0 → max → 0 sweep, both directions by default, for the effort→speed map. Takes command-line options; see `--help`.
- `control_panel.py` plus `control_panel/index.html`: browser control panel at `http://127.0.0.1:8080` (localhost only, by user choice).
  - Live data for motors 1-4 with charts (10 s / 30 s / 2 min window, Pause), Set-then-Apply effort commands, per-motor Stop, and STOP ALL (also the Esc key). STOP ALL also aborts tests.
  - Recording: optional per-motor CSV recording (`*_manual_panel.csv`), and Record all, which starts every online motor with shared notes and a `record_group` ID in the metadata. Different Teensys' clocks aren't synchronized, so files in a group line up only to about one packet (50 ms).
  - Each card has "Run a test" (step or sweep from `test_profiles.py`) with a live preview, a confirm dialog and a progress bar. While a test runs it owns that motor: manual commands and its recording are locked. It aborts on Stop, STOP ALL, a closed page, server shutdown, or 2 s without data, and partial CSVs are always saved.
  - Panel-added features so far are the user's picks (tests, Record all, chart controls). Suggested but not built: auto safety stops (overspeed, stall/dead encoder), temperature readings, a session log, and pair mode.
  - The server is stdlib Python plus paho, and the page has no external dependencies.
  - Each card has an Electrical row (voltage, current, power = V×I). It shows "sensors not wired" until that motor's ID is added to `POWER_SENSORS` in `control_panel.py`, after the sensors are wired and `currentScale`/`voltageScale` are calibrated in the firmware. Today V/I come from the packet header at about 20 Hz.
  - The user doesn't want an instant-cut command; the HV killswitch is the emergency stop.
  - Safety: commands are published only while a target is non-zero, so an idle panel doesn't fight scripts. If no browser has been connected for 1 s, it stops the motors it was driving and saves recordings. POSTs are checked against Host, Origin and JSON content type.
- Run the scripts from the repo root, since `data/` is a relative path. Results go in `data/motor_<id>/*.csv`.
- `data/`: recorded runs, in three formats:
  - before 2026-11-02 18:00 UTC: old firmware, position/velocity were placeholders;
  - 2026-11-02 18:1x: 5 Hz, one row per packet;
  - later: 1 kHz, one row per sample.
- `data/` is git-ignored (user's choice; 1 kHz runs are about 9 MB each). The CSVs committed before that rule stay tracked, so checking out old commits doesn't delete files from disk.
- Git tag `motor1-working-5hz` marks the last 5 Hz version.

## MQTT interface

- Broker: `192.168.10.1:1883` (this PC). Teensys get their IP via DHCP.
- `robot/motor/<id>/command`: one little-endian float32 effort in [-1, 1].
- `robot/motor/<id>/state`: batched packet (version 2) at 20 Hz.
  - Header `'<HHIIff'` (20 B): `version, sample_count, sequence, dropped_samples, current, voltage`.
  - Then 50 samples of `'<Iif'` (12 B each): `timestamp_us, encoder_count, effort_out`, one per ms.
  - Position is sent as raw int32 counts; Python converts to radians in double precision.
  - Velocity is computed on the PC. CSVs use a centered difference over 10 ms; raw counts are always logged too.
  - A `sequence` gap means MQTT lost a packet. A rising `dropped_samples` means the Teensy's buffer overflowed.
  - Current and voltage are batch-rate placeholders; the sensors aren't wired up or calibrated yet.
  - The firmware and `utils/data_manager.py` must match. `PACKET_VERSION` is checked, and a mismatch prints "reflash the Teensy".

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
