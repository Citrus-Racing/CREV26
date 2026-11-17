# Test Bench Roadmap

> **Last updated 2026-11-17.** Open questions are at the bottom.

## Goal

**Project 1 (this roadmap): the pushcart test bench.**

- **Hardware:** a pushcart carrying four 3 kW BLDC scooter motors (corners LF, RF, LR, RR) and four motor controllers.
- **Electronics:** four Teensy 4.1 control-and-sensor computers, and one main Ubuntu computer they report to over Ethernet/MQTT.
- **Finished when:** all four motors are **characterized under load**, with models validated against measured data.
- **No deadline.** Work through the phases in order; a later phase can start
  early only if it doesn't wait on an earlier one.

**Project 2 (out of scope here): a small go-kart using these motors.**

It doesn't start until Project 1 is finished. Don't add go-kart tasks to this
file; note ideas for it at the bottom instead.

**Keep Project 1 go-kart compatible.** The go-kart will reuse the same motors,
controllers, Teensys and software, so:

- Model each **motor and controller as one unit**, including the controller's
  dead band, throttle mapping and reverse limit. Those quirks will be on the kart too.
- Don't tie software to this particular PC. The kart will run it on a mobile
  Ubuntu computer.
- Keep the MQTT design open to a fifth node. The kart will add a chassis Teensy
  for driver inputs and frame monitoring.

## Team

| Name | Role | Focus |
|---|---|---|
| | Electrical advisor | Reviews electrical and HV work |
| | Student | |
| | Student | |
| | Student (joined 2026-11-17) | |

_Fill in names. Add yourself if you join._

**Electrical sign-off:** tasks marked ⚡ touch HV or power wiring. Don't mark
them `done` until the electrical advisor has reviewed the work and added their
initials to Status.

## How to use this file

- **Pick a task:** put your name in its `Owner` field and set `Status` to `in progress`.
  Commit that edit before you start, so two people don't take the same task.
- **Status values:** `todo`, `in progress`, `blocked (why)`, `done (date)`.
- **Done when** says what has to be true before a task counts as finished. If you
  think it's wrong, change it in the same commit and say why.
- **Add tasks freely.** Put each one under the phase it unblocks.
- **Safety:** before you command a motor, read *Bench hardware notes* and *Working
  rules* in `CLAUDE.md`. The emergency-off button on top of the cart is the emergency stop.

## Where we are (2026-11-17)

Working:

- All four Teensys run the same generated firmware.
  - It samples the encoder and sensors at 1 kHz.
  - It publishes over MQTT at 20 Hz using packet v3.
- Browser control panel: live charts, effort commands, CSV recording, and step and sweep tests.
- LF: encoder and drive verified end to end. A forward effort→speed map was measured on 2026-11-02.
- Current sensors (WCS1600) are wired on all four corners. They read about 0 A at rest but are **uncalibrated**.

Not done yet:

- RF, LR and RR haven't had their encoder direction checked.
- There are no voltage sensors, so electrical power can't be computed.
- Each encoder runs off a temporary 9 V battery.
- No motor model has been fitted yet.
- No loading between motors.
- **The power path has known safety gaps.** See Track H, which comes before any capacitor-bank or loaded work.

As built (2026-11-17):

- Motors, controllers and Teensys sit on the pushcart. Signal wiring is on solderless breadboards.
- **HV pack:** six cheap 12 V LFP batteries in series, about 77 V nominal, on the bottom of the cart.
  - Each battery has a built-in 20 A BMS. There's no datasheet, so the series limit is unknown.
  - Before being put in series, they were charged one at a time with a 12 V charger. As a string, they've been charged once with a 72 V charger.
- **Switching:** battery + goes to a normally open relay (TE HCR 150, V23132-A2001-A200, 12 V coil). From there it runs to the screw-terminal distribution under the cart top.
  - The emergency-off button on top switches the relay coil.
  - The coil runs from a 12 V power brick on the cart's power strip.
- **Guarding:** the encoder holders cover the spinning shafts. There are no chains yet.
- **Capacitor bank (owned, not in use yet):** 36 × 500 F supercaps on breadboards, 6 per board, with balancing.

---

## Phase 0: Bring the bench back up after the rewire

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| 0.1 | Check every board after the rewire | `python3 firmware/find_boards.py` exits 0, and all four `robot/motor/<id>/state` topics publish | | todo |
| 0.2 | Verify `ENCODER_SIGN` on RF, LR, RR | For each corner, positive effort reads as positive velocity. `CORNERS` in `firmware/make_corner_sketches.py` is updated and the boards are reflashed | | todo |
| 0.3 | Re-measure current-sensor zeros | Each corner's pin 21 is measured with the motor stopped, and `zero_v` in `utils/sensors.py` is updated. RF's value is a guess today | | todo |
| 0.4 | Permanent encoder supply ⚡ | All four encoders run from one 9–24 V supply with its negative tied to Teensy GND. No batteries | | todo |

## Track P: Pushcart build (runs alongside phases 0–3, must finish before phase 4)

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| P.1 | Document the as-built cart | Photos and a wiring diagram in `docs/` show every pin, connector, fuse and HV path on the cart, kept current after each change | | todo |
| P.2 | Chain guard | Every chain and sprocket is fully enclosed before any chain is installed or run | | todo |
| P.3 | Get signal wiring off solderless breadboards ⚡ | Teensy, controller signal and sensor wiring is soldered (protoboard, or a PCB once there's budget) with strain relief, labeled per corner | | todo |
| P.4 | Motor mounts for pairing | Shafts of each pair are aligned for chain coupling, with mounts rigid enough for loaded runs | | todo |

## Track H: HV power safety ⚡ (before Track C and phase 4)

Every task here needs the electrical advisor's sign-off.

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| H.1 | E-stop relay rated for the bus | The HCR 150 is a 12 V automotive relay; TE rates its contacts at 12 VDC, so opening it under load at about 77 V may arc or weld the contacts. It's replaced by a DC contactor rated to break at least 100 VDC at our current, or the advisor has documented why it's acceptable | | todo |
| H.2 | Main DC fuse | A fuse rated for at least 100 VDC sits close to battery +, ahead of everything else, with a fuse per controller at the distribution terminals | | todo |
| H.3 | E-stop also stops the controllers | The button's second contact (or a relay) cuts each controller's ignition/enable line, so the controllers stop driving before the HV relay has to break current | | todo |
| H.4 | Battery check procedure | A written procedure measures each 12 V battery separately and tops up any that differ, using the 12 V charger. It's run and logged before test days | | todo |
| H.5 | HV ground map | It's measured and written down whether Teensy GND connects to battery − (for example through the controller throttle ground), and what that means when a Teensy is also plugged into the PC by USB | | todo |
| H.6 | Safe bus voltage monitoring | A reviewed design (divider or isolated amplifier) feeds pin 20. It's protected so a fault can't put HV on a Teensy pin or the PC, and the panel shows bus V. This replaces task 1.2 | | todo |

## Track C: Run the motors from the capacitor bank ⚡ (after Track H, before phase 4)

The plan: a buck charger limited to 10 A and 72 V charges the supercap bank from
the battery pack, and the motors run from the bank. The bank supplies peak
current beyond the batteries' 20 A BMS limit. Bus voltage will swing during runs,
including from regenerative braking, and we want to measure that swing.

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| C.1 | Characterize the bank on paper | The cell voltage rating, balancer threshold and series/parallel layout are written down, with the total capacitance, maximum voltage and stored energy | | todo |
| C.2 | Bank wiring rated for motor current | Cells are joined by soldered or bolted bus bars sized for the peak motor current. Solderless breadboard contacts (about 1 A) carry no motor current | | todo |
| C.3 | Discharge and indication | A bleed resistor and a visible voltage indicator are on the bank, with a written procedure to discharge it and prove it dead before anyone touches it | | todo |
| C.4 | Charger: design (or buy) the 10 A / 72 V buck | A reviewed schematic (KiCad, in the repo), or a bought module, does current-limited then voltage-limited charging. Input parts are rated for at least 100 V, since the pack reaches about 87 V fully charged | | todo |
| C.5 | Re-wire the E-stop for the bank | Pressing the button disconnects the bank from the controllers and switches in the bleed resistor, as well as opening the battery relay | | todo |
| C.6 | Staged charge tests | The bank is charged in steps (one board first, then more, raising the voltage limit each time), with cell voltages and temperatures logged at each step | | todo |
| C.7 | Run from the bank and measure the swing | Motors run from the bank with bus V logged at 1 kHz. A predefined abort voltage (below the caps' and controllers' limits) is enforced. The swing, including regen, is reported | | todo |

## Phase 1: Trustworthy measurements

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| 1.1 | Calibrate current sensors (gain and sign) | Each sensor is checked against a reference meter at several currents. `v_per_a` and its sign are set, and `calibrated=True` | | todo |
| 1.2 | Bus voltage in software | Entries exist in `VOLTAGE_SENSORS` for the H.6 hardware, and the panel shows V and P. The hardware is in H.6 | | todo |
| 1.3 | Calibrate voltage sensors | Readings match a meter across the 72–80 V range | | todo |
| 1.4 | Measurement uncertainty note | A short doc gives the noise and resolution of speed, current and voltage at 1 kHz and 20 Hz | | todo |

## Phase 2: Characterize each motor plus its controller

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| 2.1 | Sweep test on all four, both directions | Sweep CSVs exist for every corner, including reverse at −1.0 (untested so far) | | todo |
| 2.2 | Effort→speed map and dead band per motor | A table gives start and stop effort, slope and top speed per corner and direction | | todo |
| 2.3 | Step responses | Step CSVs exist at several efforts per corner (ramped, because the controller rejects hard steps) | | todo |
| 2.4 | Investigate the 3.3 V throttle cap ⚡ | It's decided whether to level-shift or DAC the throttle to reach full scale (about 4900 rpm expected, 3769 rpm measured) | | todo |

## Phase 3: Motor models (no load)

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| 3.1 | Choose model structure | The model form and parameters are written down (for example: controller map, then DC-motor-equivalent first-order dynamics) | | todo |
| 3.2 | Fitting scripts | A script in the repo fits the parameters from the CSVs | | todo |
| 3.3 | Validation | The model predicts held-out runs within an error target we agree on | | todo |

## Phase 4: Loaded testing (motor pairs)

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| 4.1 | Mechanical coupling design | A coupling design is reviewed for each pair, with guarding | | todo |
| 4.2 | Automatic safety stops | The panel stops on overspeed, a stalled motor or a dead encoder | | todo |
| 4.3 | Time sync between Teensys | Paired runs line up better than the current ~50 ms | | todo |
| 4.4 | Pair test mode | One motor drives while the other loads, and both are recorded as a group | | todo |
| 4.5 | Loaded models | The phase 3 models are extended with torque and current under load | | todo |

## Ongoing / infrastructure

| # | Task | Done when | Owner | Status |
|---|---|---|---|---|
| I.1 | Student onboarding README (**do first**: a new member just joined) | A new member can set up, run the panel and record a run without help | | todo |
| I.2 | Shared data storage | There is an agreed place for CSVs (`data/` is git-ignored) | | todo |
| I.3 | Git workflow | Branch and review rules for multiple contributors are written down | | todo |

---

## Go-kart ideas (Project 2, parked)

What we expect so far (nothing is planned yet):

- Same motors, controllers and corner Teensys as the pushcart.
- An extra **chassis Teensy** for driver inputs (throttle, brake, steering) and frame monitoring.
- A **mobile Ubuntu computer** as the main node.

_Add notes here._

## Open questions

1. What cell voltage rating and balancer type are on the supercap boards? (Needed for C.1.)
