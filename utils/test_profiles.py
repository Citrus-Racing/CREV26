"""Test definitions shared by the terminal scripts and the control panel.

Each builder validates its parameters (raising ValueError with a readable
message) and returns a TestPlan: an effort-vs-time profile plus the metadata
used to name and label the CSV. A test therefore produces the same file
whether it was started from a script or from the panel.
"""

import math
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List

FIRMWARE_RAMP_PER_S = 0.25   # firmware RAMP_UP_PER_S; faster profiles lag behind it
CONTROLLER_DEADBAND = 0.42   # motor won't start below this |effort| (LF, old motor 1, measured)
SWEEP_REST_S = 2.0           # at 0 before/after each sweep so the motor fully stops
MAX_TEST_S = 3600.0


@dataclass
class TestPlan:
    profile: Callable[[float], float]    # t (s since start) -> effort
    duration_s: float
    test_type: str                       # goes in the CSV name and metadata
    tag: str                             # goes in the CSV name
    params: Dict[str, Any]               # written to the CSV metadata
    description: str
    warnings: List[str] = field(default_factory=list)


def _number(value, name: str, lo: float, hi: float) -> float:
    if isinstance(value, bool):
        raise ValueError(f"{name} must be a number")
    try:
        x = float(value)
    except (TypeError, ValueError):
        raise ValueError(f"{name} must be a number") from None
    if not math.isfinite(x) or not lo <= x <= hi:
        raise ValueError(f"{name} must be between {lo:g} and {hi:g}")
    return x


def step_plan(effort, duration) -> TestPlan:
    """Command `effort` for `duration` s, then 0 for `duration` s."""
    effort = _number(effort, "effort", -1.0, 1.0)
    duration = _number(duration, "duration", 0.5, MAX_TEST_S / 2)

    warnings = []
    if abs(effort) < CONTROLLER_DEADBAND:
        warnings.append(f"|effort| is below the controller dead band (~{CONTROLLER_DEADBAND}); "
                        f"the motor may not turn")
    warnings.append(f"the firmware ramp ({FIRMWARE_RAMP_PER_S}/s) shapes the step")

    return TestPlan(
        profile=lambda t: effort if t < duration else 0.0,
        duration_s=2 * duration,
        test_type="stepresponse",
        tag=f"effort{effort:.2f}_duration{duration:.0f}s",
        params={"effort_level": effort, "duration_seconds": duration},
        description=f"Step to {effort:+.2f} for {duration:g} s, then 0 for {duration:g} s",
        warnings=warnings,
    )


def sweep_plan(max_effort, ramp_time, hold, direction) -> TestPlan:
    """Ramp 0 -> peak -> 0 for each direction, with rests at 0 in between."""
    max_effort = _number(max_effort, "max effort", 0.01, 1.0)
    ramp_time = _number(ramp_time, "ramp time", 0.5, 1800.0)
    hold = _number(hold, "hold", 0.0, 1800.0)
    signs = {"both": [1, -1], "forward": [1], "reverse": [-1]}.get(direction)
    if signs is None:
        raise ValueError("direction must be both, forward or reverse")

    segments = []   # (duration_s, start_effort, end_effort)
    for sign in signs:
        peak = sign * max_effort
        segments += [(SWEEP_REST_S, 0.0, 0.0), (ramp_time, 0.0, peak),
                     (hold, peak, peak), (ramp_time, peak, 0.0)]
    segments.append((SWEEP_REST_S, 0.0, 0.0))
    total = sum(d for d, _, _ in segments)
    if total > MAX_TEST_S:
        raise ValueError(f"test would take {total:.0f} s; the limit is {MAX_TEST_S:.0f} s")

    def profile(t: float) -> float:
        for duration, start, end in segments:
            if t < duration:
                return start + (end - start) * (t / duration)
            t -= duration
        return 0.0

    warnings = []
    rate = max_effort / ramp_time
    if rate > FIRMWARE_RAMP_PER_S:
        warnings.append(f"sweep rate {rate:.3f}/s is faster than the firmware ramp "
                        f"({FIRMWARE_RAMP_PER_S}/s); the output will lag the command")

    path = {"both": f"0 → +{max_effort:.2f} → 0 → −{max_effort:.2f} → 0",
            "forward": f"0 → +{max_effort:.2f} → 0",
            "reverse": f"0 → −{max_effort:.2f} → 0"}[direction]
    return TestPlan(
        profile=profile,
        duration_s=total,
        test_type="sweep",
        tag=f"{direction}_max{max_effort:.2f}_ramp{ramp_time:.0f}s",
        params={"max_effort": max_effort, "ramp_time_s": ramp_time,
                "hold_s": hold, "direction": direction},
        description=f"Sweep {path}, {ramp_time:g} s ramps, hold {hold:g} s",
        warnings=warnings,
    )
