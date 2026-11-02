"""State-packet parsing and CSV logging for the motor nodes.

Packet format (version 2, little-endian, must match firmware/motor_node):
  header  '<HHIIff' (20 bytes): version, sample_count, sequence, dropped_samples,
                                current, voltage   (current/voltage: batch-rate, uncalibrated)
  samples '<Iif'    (12 bytes each): timestamp_us, encoder_count, effort
"""

import csv
import math
import os
import struct
import threading
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Dict, List, Tuple

# Must match firmware: 600 P/R encoder, 4x decoding by the Encoder library
ENCODER_COUNTS_PER_REV = 2400
RAD_PER_COUNT = 2.0 * math.pi / ENCODER_COUNTS_PER_REV

PACKET_VERSION = 2
HEADER_FMT = '<HHIIff'
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 20
SAMPLE_FMT = '<Iif'
SAMPLE_SIZE = struct.calcsize(SAMPLE_FMT)   # 12
NOMINAL_SAMPLE_RATE_HZ = 1000

# CSV velocity column: centered difference over +/- this many samples
# (5 -> 10 ms window at 1 kHz, no phase lag). Raw counts are always logged too.
VELOCITY_HALF_WINDOW = 5


@dataclass
class StatePacket:
    sequence: int
    dropped_samples: int
    current: float
    voltage: float
    samples: List[Tuple[int, int, float]]   # (timestamp_us, encoder_count, effort)

    def velocity_rad_s(self) -> float:
        """Average shaft velocity across this packet (first to last sample)."""
        (t0, c0, _), (t1, c1, _) = self.samples[0], self.samples[-1]
        dt = ((t1 - t0) & 0xFFFFFFFF) * 1e-6
        return (c1 - c0) * RAD_PER_COUNT / dt if dt > 0 else 0.0


def parse_state_packet(payload: bytes) -> StatePacket:
    """Parse one batched state packet from a motor node. Raises ValueError if malformed."""
    if len(payload) < HEADER_SIZE:
        raise ValueError(f"Packet too short ({len(payload)} bytes)")
    version, n, sequence, dropped, current, voltage = struct.unpack_from(HEADER_FMT, payload)
    if version != PACKET_VERSION:
        raise ValueError(f"Packet version {version} != {PACKET_VERSION} "
                         f"({len(payload)} bytes). Firmware and utils/ don't match; reflash the Teensy.")
    if len(payload) != HEADER_SIZE + n * SAMPLE_SIZE or n == 0:
        raise ValueError(f"Packet says {n} samples but is {len(payload)} bytes")
    samples = list(struct.iter_unpack(SAMPLE_FMT, payload[HEADER_SIZE:]))
    return StatePacket(sequence, dropped, current, voltage, samples)


class DataManager:
    """Collects samples from state packets and writes them to a CSV with a metadata header.

    log_packet() is called from the MQTT thread; write_file() from the main thread.
    """

    def __init__(self, data_dir: str = "data"):
        self.data_dir = data_dir
        self.csv_file = None
        self.metadata: Dict[str, Any] = {}
        self._lock = threading.Lock()
        # rows: (timestamp_us_raw, timestamp_us_unwrapped, encoder_count, effort, current, voltage)
        self._rows: List[Tuple[int, int, int, float, float, float]] = []
        self._last_seq = None
        self._packets = 0
        self._packets_lost = 0
        self._dropped_first = None
        self._dropped_last = None
        self._last_ts = None
        self._ts_offset = 0

    @property
    def has_data(self) -> bool:
        return bool(self._rows)

    def create_test_file(self, motor_id: int, test_type: str, tag: str,
                         params: Dict[str, Any], notes: str = "",
                         broker_ip: str = "192.168.10.1") -> str:
        """Choose the CSV path for this run and record its metadata. Nothing is written yet."""
        motor_dir = os.path.join(self.data_dir, f"motor_{motor_id}")
        os.makedirs(motor_dir, exist_ok=True)

        now = datetime.now(timezone.utc)
        filename = f"{now:%Y-%m-%dT%H-%M-%S}_motor{motor_id}_{test_type}_{tag}.csv"
        self.csv_file = os.path.join(motor_dir, filename)

        self.metadata = {
            "timestamp": now.isoformat(timespec="seconds"),
            "motor_id": motor_id,
            "test_type": test_type,
            **params,
            "nominal_sample_rate_hz": NOMINAL_SAMPLE_RATE_HZ,
            "encoder_counts_per_rev": ENCODER_COUNTS_PER_REV,
            "velocity_window_ms": 2 * VELOCITY_HALF_WINDOW * 1000 / NOMINAL_SAMPLE_RATE_HZ,
            "broker_ip": broker_ip,
            "notes": notes,
        }
        return self.csv_file

    def log_packet(self, packet: StatePacket):
        """Append every sample in a state packet (thread-safe)."""
        with self._lock:
            if self._last_seq is not None:
                gap = (packet.sequence - self._last_seq - 1) & 0xFFFFFFFF
                if gap < 1_000_000:          # ignore a reset (Teensy rebooted mid-test)
                    self._packets_lost += gap
            self._last_seq = packet.sequence
            self._packets += 1
            if self._dropped_first is None:
                self._dropped_first = packet.dropped_samples
            self._dropped_last = packet.dropped_samples

            for ts, count, effort in packet.samples:
                # micros() wraps every ~71.6 min; keep time monotonic across a wrap
                if self._last_ts is not None and self._last_ts - ts > 2**31:
                    self._ts_offset += 2**32
                self._last_ts = ts
                self._rows.append((ts, ts + self._ts_offset, count, effort,
                                   packet.current, packet.voltage))

    def summary(self) -> Dict[str, Any]:
        with self._lock:
            n = len(self._rows)
            duration = (self._rows[-1][1] - self._rows[0][1]) * 1e-6 if n > 1 else 0.0
            return {
                "samples": n,
                "duration_s": round(duration, 3),
                "measured_sample_rate_hz": round((n - 1) / duration, 1) if duration > 0 else 0,
                "packets": self._packets,
                "packets_lost": self._packets_lost,
                "teensy_dropped_samples": (self._dropped_last - self._dropped_first)
                                          if self._dropped_first is not None else 0,
            }

    def write_file(self):
        """Write all logged samples to the CSV, with metadata + run summary in the header."""
        if not self.csv_file:
            raise ValueError("No CSV file created. Call create_test_file() first.")
        summary = self.summary()
        with self._lock:
            rows = list(self._rows)
        if not rows:
            print("⚠ No samples recorded; nothing written")
            return

        t0 = rows[0][1]
        times = [(r[1] - t0) * 1e-6 for r in rows]
        counts = [r[2] for r in rows]
        W = VELOCITY_HALF_WINDOW

        with open(self.csv_file, 'w', newline='') as f:
            f.write("# METADATA\n")
            for key, value in {**self.metadata, **summary}.items():
                f.write(f"# {key}: {value}\n")
            f.write("#\n")
            f.write("# DATA\n")

            writer = csv.writer(f)
            writer.writerow(["time_s", "timestamp_us", "effort", "encoder_count",
                             "position_rad", "velocity_rad_s", "current_A", "voltage_V"])
            for i, (ts_raw, _, count, effort, current, voltage) in enumerate(rows):
                vel = ""
                if W <= i < len(rows) - W:
                    dt = times[i + W] - times[i - W]
                    if dt > 0:
                        vel = f"{(counts[i + W] - counts[i - W]) * RAD_PER_COUNT / dt:.4f}"
                writer.writerow([f"{times[i]:.6f}", ts_raw, f"{effort:.5f}", count,
                                 f"{count * RAD_PER_COUNT:.6f}", vel,
                                 f"{current:.3f}", f"{voltage:.3f}"])

        print(f"✓ Data saved to: {self.csv_file}")
        print(f"  {summary['samples']} samples over {summary['duration_s']} s "
              f"(~{summary['measured_sample_rate_hz']} Hz), "
              f"packets lost: {summary['packets_lost']}, "
              f"Teensy-dropped samples: {summary['teensy_dropped_samples']}")
