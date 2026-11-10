"""Current/voltage sensor calibration, per wheel corner.

The firmware sends the raw sensor-pin voltages (0-3.3 V at the Teensy ADC, averaged
over each 50 ms packet). Converting them to amps and volts happens here, so
calibrating a sensor means editing this table and restarting the panel or script.
No reflash is needed. CSVs log the raw pin voltages too, so a recording can be
recomputed after the calibration changes.

A corner that isn't listed shows "not wired" in the control panel, and its
current_A / voltage_V columns are left empty in CSVs.
"""

import math
from dataclasses import dataclass
from typing import Optional


@dataclass(frozen=True)
class CurrentSensor:
    zero_v: float       # pin voltage at 0 A
    v_per_a: float      # sensitivity, V per A. Flip the sign if current reads backwards.
    calibrated: bool    # False -> the panel tags the reading "uncalibrated"
    note: str

    def amps(self, pin_v: Optional[float]) -> Optional[float]:
        if pin_v is None or math.isnan(pin_v):
            return None
        return (pin_v - self.zero_v) / self.v_per_a


@dataclass(frozen=True)
class VoltageSensor:
    volts_per_pin_v: float   # bus volts per volt at the pin (divider ratio)
    offset_v: float          # bus volts at 0 V on the pin
    calibrated: bool
    note: str

    def volts(self, pin_v: Optional[float]) -> Optional[float]:
        if pin_v is None or math.isnan(pin_v):
            return None
        return pin_v * self.volts_per_pin_v + self.offset_v


# WCS1600 hall sensor (MCP602 op-amp board) on the Teensy's 3.3 V. The datasheet gives
# 22 mV/A at 5 V and says the output is ratiometric, so ~14.5 mV/A at 3.3 V.
# The board's op-amp could change that, so treat it as a starting point until calibrated.
# zero_v is pin 21 measured with the motor stopped on 2026-11-10.
WCS1600_3V3_V_PER_A = 0.022 * 3.3 / 5.0

CURRENT_SENSORS = {
    "LF": CurrentSensor(zero_v=1.708, v_per_a=WCS1600_3V3_V_PER_A, calibrated=False,
                        note="WCS1600 at 3.3 V, datasheet sensitivity, zero measured 2026-11-10"),
}

# Bus voltage sensors (pin 20). None are wired yet.
VOLTAGE_SENSORS = {}


def current_sensor(motor_id: str) -> Optional[CurrentSensor]:
    return CURRENT_SENSORS.get(motor_id)


def voltage_sensor(motor_id: str) -> Optional[VoltageSensor]:
    return VOLTAGE_SENSORS.get(motor_id)


def describe(motor_id: str) -> dict:
    """Calibration in use, for CSV metadata."""
    cs, vs = current_sensor(motor_id), voltage_sensor(motor_id)
    return {
        "current_sensor": (f"zero_v={cs.zero_v} v_per_a={cs.v_per_a:.6g} "
                           f"calibrated={cs.calibrated} ({cs.note})") if cs else "none",
        "voltage_sensor": (f"volts_per_pin_v={vs.volts_per_pin_v:.6g} offset_v={vs.offset_v} "
                           f"calibrated={vs.calibrated} ({vs.note})") if vs else "none",
    }
