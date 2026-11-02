import csv
import json
import os
from datetime import datetime
from typing import Dict, List, Any
import math
import struct

# Must match firmware: 600 P/R encoder, 4x decoding by the Encoder library
ENCODER_COUNTS_PER_REV = 2400
RAD_PER_COUNT = 2.0 * math.pi / ENCODER_COUNTS_PER_REV

class DataManager:
    """Manages CSV file creation and data logging with embedded metadata."""
    
    def __init__(self, data_dir: str = "data"):
        self.data_dir = data_dir
        self.csv_file = None
        self.csv_writer = None
        self.metadata: Dict[str, Any] = {}
        self.rows: List[Dict[str, Any]] = []
    
    def create_test_file(self, motor_id: int, effort: float, duration: float, 
                         notes: str = "", broker_ip: str = "192.168.10.1"):
        """Create a new CSV file for this test run."""
        
        # Create directory structure
        motor_dir = os.path.join(self.data_dir, f"motor_{motor_id}")
        os.makedirs(motor_dir, exist_ok=True)
        
        # Generate filename with timestamp
        now = datetime.utcnow()
        timestamp_str = now.strftime("%Y-%m-%dT%H-%M-%S")
        filename = f"{timestamp_str}_motor{motor_id}_stepresponse_effort{effort:.2f}_duration{duration:.0f}s.csv"
        filepath = os.path.join(motor_dir, filename)
        
        # Store metadata
        self.metadata = {
            "timestamp": now.isoformat() + "Z",
            "motor_id": motor_id,
            "test_type": "step_response",
            "effort_level": effort,
            "duration_seconds": duration,
            "publishing_frequency_hz": 5,
            "broker_ip": broker_ip,
            "encoder_counts_per_rev": ENCODER_COUNTS_PER_REV,
            "notes": notes,
        }
        
        self.csv_file = filepath
        return filepath
    
    def log_state(self, timestamp_us: int, effort_command: float, current: float, 
                  voltage: float, encoder_count: int, position: float, velocity: float):
        """Log a motor state measurement."""
        self.rows.append({
            "timestamp_us": timestamp_us,
            "effort_command": effort_command,
            "current_A": current,
            "voltage_V": voltage,
            "encoder_count": encoder_count,
            "position_rad": position,
            "velocity_rad_s": velocity,
        })
    
    def write_file(self):
        """Write all logged data to CSV with metadata header."""
        if not self.csv_file:
            raise ValueError("No CSV file created. Call create_test_file() first.")
        
        with open(self.csv_file, 'w', newline='') as f:
            # Write metadata header
            f.write("# METADATA\n")
            for key, value in self.metadata.items():
                f.write(f"# {key}: {value}\n")
            f.write("#\n")
            f.write("# DATA\n")
            
            # Write data rows
            fieldnames = ["timestamp_us", "effort_command", "current_A", "voltage_V",
                          "encoder_count", "position_rad", "velocity_rad_s"]
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(self.rows)
        
        print(f"✓ Data saved to: {self.csv_file}")
    
    def clear(self):
        """Clear logged data for a new test."""
        self.rows = []
        self.metadata = {}
        self.csv_file = None

def parse_motor_state(payload: bytes) -> tuple:
    """Unpack a 24-byte MotorState struct from Teensy.
    
    Struct layout (matches Teensy C struct):
      unsigned long timeStamp      (4 bytes)  - U
      float effortCommand          (4 bytes)  - f
      float current                (4 bytes)  - f
      float voltage                (4 bytes)  - f
      int32_t encoderCount         (4 bytes)  - i
      float velocity               (4 bytes)  - f

    Returns: (timestamp_us, effort_command, current_A, voltage_V,
              encoder_count, position_rad, velocity_rad_s)
    position_rad is computed here from encoder_count in double precision.
    """
    if len(payload) != 24:
        raise ValueError(f"Expected 24 bytes, got {len(payload)}")

    # '<' = little-endian, no padding (matches the packed struct on the Teensy).
    # uint32 + 3 floats + int32 + float = 24 bytes.
    timestamp_us, effort, current, voltage, count, velocity = struct.unpack('<Ifffif', payload)
    return timestamp_us, effort, current, voltage, count, count * RAD_PER_COUNT, velocity

