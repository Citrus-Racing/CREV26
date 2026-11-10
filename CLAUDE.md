# CLAUDE.md

I am making a test bench for a new fsae EV division. the test bench has 4 motors that are currently free spinning, but we will later link pairs together to have them fight against each other to simulate load. The goal is to sent enough impulses to build accurate motor models. We have external encoders that are mounted right to each motor. The setup is each motor has its own mcu (a teensy) that is connected to this PC over ethernet which is the main node. The MCUs are responsible for reading the state of the motor (right now just the encoder but eventually the turn angle and power going into/out of the motor) as well as reading a command from the host PC to send a pwm signal to the motor controller.

If you ever have any questions about anything related to the setup just ask.

## Repo layout

- Motors are named by wheel corner: `LF`, `RF`, `LR`, `RR` (`utils/motors.py` holds the list and topic helpers). LF is the board formerly called motor 1.
- `firmware/motor_node/motor_node.ino`: Teensy 4.1 firmware **template**. It has an `#error`, so it can't be flashed by mistake.
  - Edit firmware here, then run `python3 firmware/make_corner_sketches.py`. That regenerates `firmware/motor_node_<corner>/motor_node_<corner>.ino` for all 4 corners. Those are what gets flashed, from the Arduino IDE or by Claude with `arduino-cli` (in `~/.local/bin`, using the IDE's `teensy:avr` core; see Working rules for when Claude may flash).
  - Per-corner values (MAC byte, `ENCODER_SIGN`) live in the `CORNERS` table in that script. The generated sketches differ only in that block.
  - `BOARD_SERIALS` in the same script maps each corner to its Teensy's factory USB serial number. That number never changes, but `/dev/ttyACM*` numbers and USB port addresses move when boards are replugged. It identifies the Teensy, not the motor, so update it if a board moves to another motor or is replaced.
  - `python3 firmware/find_boards.py` (read-only) finds each corner's board by serial number. It prints its serial device and `arduino-cli` upload port, checks that it's running its own `Motor_<corner>` firmware, and exits 1 on any problem.
  - Each sketch also gets a generated `name.c` that sets the USB product name to `Motor_<corner>`, so `/dev/serial/by-id/` shows which corner a board is running.
  - A 1 kHz `IntervalTimer` interrupt does the control update (watchdog, ramp, PWM) and the encoder sample into a ring buffer. `loop()` handles networking and samples the current/voltage sensor pins (12-bit, ~1 kHz, averaged per packet). Those reads can't go in the ISR, because `analogRead()` calls `yield()`.
- `firmware/sensor_test_LF/`: standalone current-sensor test for LF, flashed in place of `motor_node_LF`. It uses no Ethernet or MQTT and talks over USB serial at 115200.
  - Every 10 s, with the motor off, it checks pin 21 with the internal pull-up and pull-down to see whether the sensor is really driving it, and scans the other free analog pins for a driven signal.
  - When the user types `r` (or `r 0.7`) in the Serial Monitor, it ramps LF forward at 0.25/s, holds, ramps down, and reports how far the sensor output moved from its zero-current reading. Claude must never send `r`, since that commands the motor.
  - While it's on the board, LF is offline and `find_boards.py` reports LF as `SensorTest_LF`. That mismatch is expected; flashing `motor_node_LF` restores it.
- `utils/data_manager.py`: state-packet parser plus CSV logging; counts lost packets and Teensy-dropped samples.
- `utils/sensors.py`: per-corner current/voltage sensor table. Each entry holds the zero point, the sensitivity, and a `calibrated` flag. The firmware sends raw pin volts, and the conversion to amps and volts happens here, so calibrating is an edit to this file plus a restart of the panel or script, with no reflash. A corner that isn't listed shows "not wired".
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
  - Each card has an Electrical row: current (with the raw pin voltage beside it), voltage, and power = V×I. Current and voltage each show "not wired" unless that motor is in `utils/sensors.py`. Power shows only when both are wired. The row is tagged "uncalibrated" until the entry's `calibrated` flag is set. Readings come from the packet header at 20 Hz, averaged over each 50 ms packet.
  - The user doesn't want an instant-cut command; the HV killswitch is the emergency stop.
  - Safety: commands are published only while a target is non-zero, so an idle panel doesn't fight scripts. If no browser has been connected for 1 s, it stops the motors it was driving and saves recordings. POSTs are checked against Host, Origin and JSON content type.
- Run the scripts from the repo root, since `data/` is a relative path. Results go in `data/motor_<id>/*.csv`.
- `data/`: recorded runs, in three formats:
  - before 2026-11-02 18:00 UTC: old firmware, position/velocity were placeholders;
  - 2026-11-02 18:1x: 5 Hz, one row per packet;
  - later: 1 kHz, one row per sample.
  - from 2026-11-10 (packet v3): also `current_sensor_V` / `voltage_sensor_V` columns, the raw pin volts, so data can be recalibrated later. `current_A` / `voltage_V` are computed from `utils/sensors.py` at record time, and the calibration used is in the metadata. Before v3, `current_A` / `voltage_V` hold meaningless placeholder values.
- `data/` is git-ignored (user's choice; 1 kHz runs are about 9 MB each). The CSVs committed before that rule stay tracked, so checking out old commits doesn't delete files from disk.
- Git tag `motor1-working-5hz` marks the last 5 Hz version.

## MQTT interface

- Broker: `192.168.10.1:1883` (this PC). Teensys get their IP via DHCP.
- `<id>` is the corner name (`LF`, `RF`, `LR`, `RR`). Before 2026-11-03 it was a number 1-4; `data/motor_1/` holds LF's older runs.
- `robot/motor/<id>/command`: one little-endian float32 effort in [-1, 1].
- `robot/motor/<id>/state`: batched packet (version 3) at 20 Hz.
  - Header `'<HHIIff'` (20 B): `version, sample_count, sequence, dropped_samples, current_sensor_v, voltage_sensor_v`.
  - The last two header fields are the sensor pin voltages (0–3.3 V), averaged over the packet. They're NaN until the first average.
  - Then 50 samples of `'<Iif'` (12 B each): `timestamp_us, encoder_count, effort_out`, one per ms.
  - Position is sent as raw int32 counts; Python converts to radians in double precision.
  - Velocity is computed on the PC. CSVs use a centered difference over 10 ms; raw counts are always logged too.
  - A `sequence` gap means MQTT lost a packet. A rising `dropped_samples` means the Teensy's buffer overflowed.
  - The firmware and `utils/data_manager.py` must match. `PACKET_VERSION` is checked, and a mismatch prints "reflash the Teensy".
  - Version 2 packets (same layout, placeholder analog fields) are still accepted, with their sensor values read as NaN. Drop `LEGACY_PACKET_VERSIONS` once all 4 boards run v3.

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
- **Current sensor** (LF only so far, 2026-11-10): a WCS1600 hall-effect sensor (the conductor passes through its core) on a board with an MCP602 op-amp, powered from the Teensy's 3.3 V.
  - AOUT goes to pin 21, GND to Teensy GND. DOUT (a comparator threshold output) stays unconnected.
  - It reads 1.708 V at the pin at 0 A.
  - Sensitivity: the datasheet gives 22 mV/A at 5 V and says the output is ratiometric, so about 14.5 mV/A at 3.3 V. That's the starting value in `utils/sensors.py`. It hasn't been calibrated yet: the board's op-amp could change it, and the sign depends on which way the wire passes through the core.
  - At 3.3 V the reading gets close to the rails from about ±100 A.
  - A loose ground or AOUT contact once made pin 21 read about 3.25 V. If it reads near 3.3 V or 0 V again, check the contacts first, then run `firmware/sensor_test_LF`.
- **Encoder**: Taiss 600 P/R optical encoder (LPD3806-600BM style), NPN open collector.
  - A goes to pin 18 and B to pin 17, each with roughly a 1 kΩ pull-up to the Teensy's 3.3 V.
  - **Power it from its own 9–24 V supply, with the supply's negative tied to Teensy GND.** Do NOT power it from the Teensy's VIN pin: on USB power VIN measured 4.3 V, below the encoder's 5 V minimum, and both outputs sat stuck low (found 2026-11-02). A 9 V battery is the temporary fix; a permanent supply is TBD.
  - Because the outputs are open collector, a higher encoder supply voltage never reaches the Teensy pins. The pull-ups set the high level, so they must stay on 3.3 V.
  - **The two pull-up resistors must not touch each other.** On motor 1 their leads were shorting, which tied A to B. The decoder then counted +2 on every edge, so velocity was always positive in both directions (found 2026-11-02).
  - Direction is set in software with `ENCODER_SIGN` in the firmware, so positive effort reads as positive velocity. LF (old motor 1) = -1, verified. RF, LR and RR default to -1 but are unverified: check each one and update `CORNERS` in `firmware/make_corner_sketches.py`.
  - With 4x decoding that's 2400 counts/rev.
  - This unit is rated for 6400 rpm (per the user's spec; generic LPD3806 listings quote lower figures, so ignore those).
  - It's mounted directly on the motor shaft.
- **Safety**: a physical killswitch on the HV lines kills everything, and the spinning parts are covered.

## Working rules

- **The user runs anything that commands a motor to move.** Claude gives the exact commands to run, and must not publish to `robot/motor/+/command` until the user explicitly hands over that control.
- Claude may passively subscribe to state topics. That's read-only and sends no commands.
- Claude may read the Teensys' serial ports and compile firmware at any time. Before reading a port, check that the Arduino IDE's Serial Monitor (`teensy-monitor`) doesn't already have it open (`fuser /dev/ttyACM*`), because two readers split the output.
- **Flashing:** Claude flashes only when the user asks. If a bigger task needs a reflash, Claude asks the user before flashing.
  - Once the user says to flash, go ahead. Don't wait for effort to read 0: the HV killswitch may be off while effort is non-zero.
  - The user will often ask to flash all 4 at once. Each board must get its own corner's sketch, so its MQTT topics, client ID, MAC byte, `ENCODER_SIGN` and USB name stay correct.
  - Steps:
    1. `python3 firmware/make_corner_sketches.py`
    2. `python3 firmware/find_boards.py`. If it reports any problem, stop and ask the user.
    3. For each corner: `arduino-cli compile --fqbn teensy:avr:teensy41 --upload -p <that corner's upload port from find_boards> firmware/motor_node_<corner>`
    4. Rerun `find_boards.py` to confirm every board shows its own `Motor_<corner>`, then check that each corner's state topic is publishing.
