#!/usr/bin/env python3
"""Control panel server for the motor test bench.

Serves control_panel/index.html at http://127.0.0.1:8080 (this PC only) and
bridges it to MQTT:
  - streams live state for motors 1-4 to the page (Server-Sent Events, 10 Hz)
  - takes effort commands from the page and publishes them at 20 Hz
  - records any motor's 1 kHz data to data/motor_<id>/*.csv, one at a time or all at once
  - runs the step / sweep tests from utils/test_profiles.py (same CSVs as the scripts)

Safety:
  - Commands are only published while a motor's target is non-zero or a test
    is running (plus a short burst of 0s after a stop), so an idle panel
    never fights a script.
  - If the last browser tab disconnects for more than LEASE_GRACE_S, the
    server aborts tests, sets every running motor to 0 and saves recordings.
  - A test aborts if its motor stops sending data for TEST_DATA_TIMEOUT_S.
  - If this server dies, the Teensy's own 1 s command watchdog stops the motor.

Usage:  python3 control_panel.py [--port 8080] [--no-browser]
"""

import argparse
import json
import math
import os
import sys
import threading
import time
import webbrowser
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPO_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(REPO_DIR, 'utils'))

from mqtt_handler import MQTTHandler
from data_manager import DataManager, parse_state_packet, ENCODER_COUNTS_PER_REV
from test_profiles import step_plan, sweep_plan

BROKER_IP = "192.168.10.1"
BROKER_PORT = 1883
MOTOR_IDS = (1, 2, 3, 4)
# Motors whose current/voltage sensors are wired AND calibrated in the firmware
# (currentScale / voltageScale). Others show "sensors not wired" on the page,
# because unconnected analog pins read meaningless values. Example: {1, 2}
POWER_SENSORS = set()
PAGE_PATH = os.path.join(REPO_DIR, "control_panel", "index.html")
DATA_DIR = os.path.join(REPO_DIR, "data")

COMMAND_RATE_HZ = 20      # well inside the firmware's 1 s watchdog
ZERO_TAIL_S = 0.5         # after a stop, keep sending 0 this long, then go quiet
EVENT_RATE_HZ = 10        # page update rate
ONLINE_TIMEOUT_S = 1.0    # no packet for this long -> motor shown offline
ERROR_HOLD_S = 2.0        # how long a bad-packet message stays visible
HISTORY_S = 130           # chart history kept on the server (page shows up to 2 min)
LEASE_GRACE_S = 1.0       # no browser connected this long -> stop motors (survives a page reload)
TEST_DATA_TIMEOUT_S = 2.0 # abort a test if its motor stops sending data this long
TEST_TAIL_S = 0.3         # keep recording this long after a test to catch in-flight packets


def log(msg: str):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Conflict(ValueError):
    """The request is valid but not allowed right now (HTTP 409)."""


class Motor:
    def __init__(self, motor_id: int):
        self.id = motor_id
        self.target = 0.0
        self.zero_until = 0.0
        self.last_packet = None          # monotonic time of last good packet
        self.count = 0
        self.effort_out = 0.0
        self.rpm = 0.0
        self.current = 0.0               # A (from the packet header, ~20 Hz)
        self.voltage = 0.0               # V
        self.last_seq = None
        self.lost = 0
        self.dropped = 0
        self.error = None
        self.error_time = 0.0
        self.points = deque(maxlen=HISTORY_S * 20)   # (idx, t, rpm, effort_out, target)
        self.next_idx = 0
        self.recorder = None             # DataManager while recording
        self.recorder_owner = None       # "manual" or "test"
        self.record_status = {"state": "idle"}
        self.test = {"state": "idle"}
        self.test_active = False         # a test thread owns this motor's commands
        self.test_abort = None           # threading.Event while a test runs
        self.test_abort_reason = None
        self.test_thread = None

    def online(self, now: float) -> bool:
        return self.last_packet is not None and now - self.last_packet < ONLINE_TIMEOUT_S


class Panel:
    def __init__(self):
        self.lock = threading.Lock()
        self.motors = {m: Motor(m) for m in MOTOR_IDS}
        self.t0 = time.monotonic()
        self.mqtt = MQTTHandler(BROKER_IP, BROKER_PORT)
        self.subscribed = False
        self.clients = 0
        self.no_clients_since = None
        self.stop_event = threading.Event()

    # ------------------------------------------------------------------ MQTT
    def start(self):
        self.mqtt.connect()
        threading.Thread(target=self._command_loop, daemon=True).start()

    def _publish(self, motor_id: int, effort: float):
        if self.mqtt.connected:
            self.mqtt.publish_effort(f"robot/motor/{motor_id}/command", effort)

    def _on_state(self, msg):
        try:
            motor = self.motors.get(int(msg.topic.split('/')[2]))
        except (IndexError, ValueError):
            return
        if motor is None:
            return
        try:
            packet = parse_state_packet(msg.payload)
        except ValueError as e:
            with self.lock:
                motor.error = str(e)
                motor.error_time = time.monotonic()
            return

        rpm = packet.velocity_rad_s() * 60.0 / (2.0 * math.pi)
        _, count, effort_out = packet.samples[-1]
        with self.lock:
            if motor.last_seq is not None:
                gap = (packet.sequence - motor.last_seq - 1) & 0xFFFFFFFF
                if gap < 1_000_000:          # ignore a Teensy reboot
                    motor.lost += gap
            motor.last_seq = packet.sequence
            motor.dropped = packet.dropped_samples
            motor.count, motor.effort_out, motor.rpm = count, effort_out, rpm
            motor.current, motor.voltage = packet.current, packet.voltage
            motor.last_packet = time.monotonic()
            motor.points.append((motor.next_idx, round(self._now(), 3), round(rpm, 1),
                                 round(effort_out, 4), motor.target))
            motor.next_idx += 1
            recorder = motor.recorder
        if recorder is not None:
            recorder.log_packet(packet)      # DataManager has its own lock

    def _command_loop(self):
        """Keeps manual targets alive at 20 Hz. Tests publish from their own thread."""
        period = 1.0 / COMMAND_RATE_HZ
        next_t = time.monotonic()
        while not self.stop_event.is_set():
            # (Re)subscribe whenever the broker connection comes (back) up
            if self.mqtt.connected and not self.subscribed:
                self.mqtt.subscribe("robot/motor/+/state", self._on_state)
                self.subscribed = True
            elif not self.mqtt.connected:
                self.subscribed = False

            now = time.monotonic()
            with self.lock:
                sends = [(m.id, m.target) for m in self.motors.values()
                         if not m.test_active and (m.target != 0.0 or now < m.zero_until)]
                lease_lost = (self.clients == 0 and self.no_clients_since is not None
                              and now - self.no_clients_since > LEASE_GRACE_S)
                if lease_lost:
                    self.no_clients_since = None
            if lease_lost:
                self.release_all("no browser connected")
            for motor_id, effort in sends:
                self._publish(motor_id, effort)

            next_t += period
            time.sleep(max(0.0, next_t - time.monotonic()))

    def _now(self) -> float:
        return time.monotonic() - self.t0

    # -------------------------------------------------------------- commands
    def _set_target(self, motor_id: int, effort: float):
        effort = max(-1.0, min(1.0, effort))
        with self.lock:
            motor = self.motors[motor_id]
            motor.target = effort
            if effort == 0.0:
                motor.zero_until = time.monotonic() + ZERO_TAIL_S
        self._publish(motor_id, effort)      # send now; the loop keeps it alive after
        log(f"motor {motor_id} -> {effort:+.2f}")

    def command(self, motor_id: int, effort: float):
        with self.lock:
            if self.motors[motor_id].test_active:
                raise Conflict("a test is running on this motor; abort it first")
        self._set_target(motor_id, effort)

    def stop(self, motor_id: int, reason: str):
        self.abort_test(motor_id, reason)
        self._set_target(motor_id, 0.0)

    def stop_all(self, reason: str):
        for motor_id in MOTOR_IDS:
            self.abort_test(motor_id, reason)
        with self.lock:
            for motor in self.motors.values():
                motor.target = 0.0
                motor.zero_until = time.monotonic() + ZERO_TAIL_S
        for motor_id in MOTOR_IDS:
            self._publish(motor_id, 0.0)
        log(f"STOP ALL ({reason})")

    def release_all(self, reason: str):
        """Lease lost: abort tests, stop motors the panel is driving, save recordings."""
        aborted = [m for m in MOTOR_IDS if self.abort_test(m, reason)]
        with self.lock:
            running = [m.id for m in self.motors.values() if m.target != 0.0 and not m.test_active]
            recording = [m.id for m in self.motors.values()
                         if m.recorder is not None and m.recorder_owner == "manual"]
        for motor_id in running:
            self._set_target(motor_id, 0.0)
        for motor_id in recording:
            self.stop_recording(motor_id)
        if aborted or running or recording:
            log(f"{reason}: aborted tests {aborted}, stopped motors {running}, "
                f"saved recordings {recording}")

    # ------------------------------------------------------------- recording
    def start_recording(self, motor_id: int, notes: str, test_type: str = "manual",
                        tag: str = "panel", params: dict = None, owner: str = "manual"):
        with self.lock:
            motor = self.motors[motor_id]
            if motor.recorder is not None:
                raise Conflict("already recording")
            if motor.record_status.get("state") == "saving":
                raise Conflict("still saving the previous recording")
            recorder = DataManager(data_dir=DATA_DIR)
            path = recorder.create_test_file(motor_id, test_type, tag,
                                             params={**(params or {}), "source": "control_panel"},
                                             notes=notes, broker_ip=BROKER_IP)
            motor.recorder = recorder
            motor.recorder_owner = owner
            motor.record_status = {"state": "recording", "file": os.path.relpath(path, REPO_DIR)}
        log(f"motor {motor_id}: recording to {os.path.relpath(path, REPO_DIR)}")

    def stop_recording(self, motor_id: int, owner: str = None, wait: bool = False):
        with self.lock:
            motor = self.motors[motor_id]
            recorder = motor.recorder
            if recorder is None:
                return
            if owner is not None and motor.recorder_owner != owner:
                raise Conflict("this recording belongs to the running test; abort the test to stop it")
            motor.recorder = None
            motor.recorder_owner = None
            rel = os.path.relpath(recorder.csv_file, REPO_DIR)
            motor.record_status = {"state": "saving", "file": rel}

        def write():
            try:
                if recorder.has_data:
                    recorder.write_file()
                    status = {"state": "saved", "file": rel, **recorder.summary()}
                else:
                    status = {"state": "error", "message": "no samples recorded (motor offline?)"}
            except Exception as e:      # report it on the page instead of losing it
                status = {"state": "error", "message": f"save failed: {e}"}
            with self.lock:
                motor.record_status = status

        # Not a daemon: Python waits for it at exit, so a save in progress
        # (e.g. a test aborted by Ctrl-C) always finishes.
        thread = threading.Thread(target=write, daemon=False)
        thread.start()
        if wait:
            thread.join()

    def record_all(self, action: str, notes: str) -> list:
        """Start/stop manual recordings on every eligible motor. Returns the motor IDs affected."""
        now = time.monotonic()
        if action == "start":
            group = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H-%M-%S")
            with self.lock:
                eligible = [m.id for m in self.motors.values()
                            if m.online(now) and m.recorder is None and not m.test_active
                            and m.record_status.get("state") != "saving"]
            if not eligible:
                raise Conflict("no online motors that aren't already recording")
            for motor_id in eligible:
                self.start_recording(motor_id, notes, params={"record_group": group})
            log(f"record all: started {eligible} (group {group})")
            return eligible
        if action == "stop":
            with self.lock:
                manual = [m.id for m in self.motors.values()
                          if m.recorder is not None and m.recorder_owner == "manual"]
            for motor_id in manual:
                self.stop_recording(motor_id)
            log(f"record all: stopped {manual}")
            return manual
        raise ValueError("action must be 'start' or 'stop'")

    # ----------------------------------------------------------------- tests
    @staticmethod
    def make_plan(kind: str, params: dict):
        if not isinstance(params, dict):
            raise ValueError("params must be an object")
        if kind == "step":
            return step_plan(params.get("effort"), params.get("duration"))
        if kind == "sweep":
            return sweep_plan(params.get("max_effort"), params.get("ramp_time"),
                              params.get("hold"), params.get("direction"))
        raise ValueError("test kind must be 'step' or 'sweep'")

    def start_test(self, motor_id: int, kind: str, params: dict, notes: str):
        plan = self.make_plan(kind, params)
        now = time.monotonic()
        with self.lock:
            motor = self.motors[motor_id]
            if motor.test_active:
                raise Conflict("a test is already running on this motor")
            if not motor.online(now):
                raise Conflict("motor is offline (no state packets)")
            if motor.recorder is not None:
                raise Conflict("stop this motor's recording first (the test records its own CSV)")
            if motor.target != 0.0:
                raise Conflict("set this motor to 0 before starting a test")
            motor.test_active = True     # claim the motor before releasing the lock
            motor.test_abort = threading.Event()
            motor.test_abort_reason = None
            motor.test = {"state": "running", "kind": kind, "description": plan.description,
                          "elapsed_s": 0.0, "duration_s": plan.duration_s, "command": 0.0}
        try:
            self.start_recording(motor_id, notes, plan.test_type, plan.tag, plan.params, owner="test")
        except Exception:
            with self.lock:
                motor.test_active = False
                motor.test = {"state": "idle"}
            raise
        motor.test_thread = threading.Thread(target=self._run_test, args=(motor, plan), daemon=True)
        motor.test_thread.start()
        log(f"motor {motor_id}: test started: {plan.description} ({plan.duration_s:g} s)")

    def _run_test(self, motor: Motor, plan):
        abort = motor.test_abort
        reason = None
        t0 = time.monotonic()
        t = 0.0
        try:
            while True:
                now = time.monotonic()
                t = now - t0
                if abort.is_set():
                    reason = motor.test_abort_reason or "aborted"
                    break
                if t >= plan.duration_s:
                    break
                with self.lock:
                    stale = motor.last_packet is None or now - motor.last_packet > TEST_DATA_TIMEOUT_S
                if stale:
                    reason = f"motor stopped sending data for {TEST_DATA_TIMEOUT_S:g} s"
                    break
                effort = plan.profile(t)
                with self.lock:
                    motor.target = effort
                    motor.test.update(elapsed_s=round(t, 1), command=round(effort, 3))
                self._publish(motor.id, effort)
                abort.wait(1.0 / COMMAND_RATE_HZ)
        except Exception as e:
            reason = f"test error: {e}"
        finally:
            with self.lock:
                motor.target = 0.0
                motor.zero_until = time.monotonic() + ZERO_TAIL_S
                motor.test_active = False    # command loop sends the zero tail from here
            self._publish(motor.id, 0.0)
            time.sleep(TEST_TAIL_S)          # catch packets still in flight
            self.stop_recording(motor.id)
            with self.lock:
                motor.test.update(state="aborted" if reason else "done",
                                  elapsed_s=round(min(t, plan.duration_s), 1), command=0.0,
                                  message=reason or "complete")
                motor.test_thread = None
            log(f"motor {motor.id}: test {'aborted (' + reason + ')' if reason else 'complete'}")

    def abort_test(self, motor_id: int, reason: str) -> bool:
        """Ask a running test to stop. Returns True if one was running."""
        with self.lock:
            motor = self.motors[motor_id]
            if not motor.test_active or motor.test_abort is None:
                return False
            motor.test_abort_reason = reason
            motor.test_abort.set()
        return True

    # -------------------------------------------------------------- browser
    def client_connected(self):
        with self.lock:
            self.clients += 1
            self.no_clients_since = None

    def client_disconnected(self):
        with self.lock:
            self.clients -= 1
            if self.clients == 0:
                self.no_clients_since = time.monotonic()

    def snapshot(self, cursors: dict) -> dict:
        """State for one page update. cursors tracks which chart points this client has."""
        now = time.monotonic()
        motors = []
        with self.lock:
            for m in self.motors.values():
                error = None
                if m.error and now - m.error_time < ERROR_HOLD_S and \
                        (m.last_packet is None or m.last_packet < m.error_time):
                    error = m.error
                last = cursors.get(m.id, -1)
                points = [list(p[1:]) for p in m.points if p[0] > last]
                if m.points:
                    cursors[m.id] = m.points[-1][0]
                record = dict(m.record_status)
                if m.recorder is not None:
                    summary = m.recorder.summary()
                    record.update(samples=summary["samples"], duration_s=summary["duration_s"])
                record["owner"] = m.recorder_owner
                wired = m.id in POWER_SENSORS
                motors.append({
                    "id": m.id,
                    "seen": m.last_packet is not None,
                    "online": m.online(now),
                    "age_ms": int((now - m.last_packet) * 1000) if m.last_packet else None,
                    "rpm": round(m.rpm, 1),
                    "effort_out": round(m.effort_out, 4),
                    "target": m.target,
                    "count": m.count,
                    "rev": round(m.count / ENCODER_COUNTS_PER_REV, 2),
                    "electrical": {
                        "wired": wired,
                        "voltage": round(m.voltage, 2) if wired else None,
                        "current": round(m.current, 2) if wired else None,
                    },
                    "lost": m.lost,
                    "dropped": m.dropped,
                    "error": error,
                    "points": points,
                    "record": record,
                    "test": dict(m.test),
                })
        return {"t": round(self._now(), 3), "broker": self.mqtt.connected, "motors": motors}

    def shutdown(self):
        self.stop_event.set()
        threads = [m.test_thread for m in self.motors.values() if m.test_thread is not None]
        for motor_id in MOTOR_IDS:
            self.abort_test(motor_id, "server shutting down")
        for thread in threads:
            thread.join(timeout=3.0)
        if self.mqtt.connected:
            for _ in range(3):
                for motor_id in MOTOR_IDS:
                    self._publish(motor_id, 0.0)
                time.sleep(0.05)
            log("Sent 0 to all motors")
        for motor_id in MOTOR_IDS:
            self.stop_recording(motor_id, wait=True)
        self.mqtt.disconnect()


panel = None   # set in main()


class Handler(BaseHTTPRequestHandler):
    server_version = "MotorBenchPanel/2"

    def log_message(self, fmt, *args):   # keep the console for commands/events
        pass

    # Only answer requests addressed to this PC's loopback name, and only accept
    # commands from our own page: blocks other websites (CSRF / DNS rebinding).
    def _host_ok(self) -> bool:
        port = self.server.server_address[1]
        return self.headers.get("Host", "") in (f"127.0.0.1:{port}", f"localhost:{port}")

    def _origin_ok(self) -> bool:
        port = self.server.server_address[1]
        origin = self.headers.get("Origin")
        return origin is None or origin in (f"http://127.0.0.1:{port}", f"http://localhost:{port}")

    def _json(self, code: int, obj: dict):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if not self._host_ok():
            return self._json(403, {"error": "forbidden host"})
        if self.path in ("/", "/index.html"):
            with open(PAGE_PATH, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            self._events()
        else:
            self._json(404, {"error": "not found"})

    def _events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        panel.client_connected()
        cursors = {}
        try:
            while not panel.stop_event.is_set():
                data = json.dumps(panel.snapshot(cursors), separators=(",", ":"))
                self.wfile.write(f"data: {data}\n\n".encode())
                self.wfile.flush()
                time.sleep(1.0 / EVENT_RATE_HZ)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        finally:
            panel.client_disconnected()

    def do_POST(self):
        if not (self._host_ok() and self._origin_ok()):
            return self._json(403, {"error": "forbidden origin"})
        if self.headers.get("Content-Type", "").split(";")[0].strip() != "application/json":
            return self._json(415, {"error": "expected application/json"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length > 10_000:
                return self._json(413, {"error": "request too large"})
            body = json.loads(self.rfile.read(length) or b"{}")
            if not isinstance(body, dict):
                raise ValueError("expected a JSON object")
        except ValueError as e:
            return self._json(400, {"error": f"bad request: {e}"})

        result = {"ok": True}
        try:
            if self.path == "/api/command":
                effort = body.get("effort")
                if isinstance(effort, bool) or not isinstance(effort, (int, float)) \
                        or not math.isfinite(effort) or not -1.0 <= effort <= 1.0:
                    raise ValueError("effort must be a number in [-1, 1]")
                panel.command(self._motor(body), float(effort))
            elif self.path == "/api/stop":
                if "motor" in body:
                    panel.stop(self._motor(body), "Stop pressed")
                else:
                    panel.stop_all("STOP ALL")
            elif self.path == "/api/record":
                motor_id = self._motor(body)
                action = body.get("action")
                if action == "start":
                    panel.start_recording(motor_id, self._notes(body))
                elif action == "stop":
                    panel.stop_recording(motor_id, owner="manual")
                else:
                    raise ValueError("action must be 'start' or 'stop'")
            elif self.path == "/api/record_all":
                result["motors"] = panel.record_all(body.get("action"), self._notes(body))
            elif self.path == "/api/test":
                action = body.get("action")
                if action == "preview":
                    plan = panel.make_plan(body.get("kind"), body.get("params"))
                    result.update(description=plan.description, duration_s=plan.duration_s,
                                  warnings=plan.warnings)
                elif action == "start":
                    panel.start_test(self._motor(body), body.get("kind"), body.get("params"),
                                     self._notes(body))
                elif action == "abort":
                    panel.stop(self._motor(body), "aborted from the panel")
                else:
                    raise ValueError("action must be 'preview', 'start' or 'abort'")
            else:
                return self._json(404, {"error": "not found"})
        except Conflict as e:
            return self._json(409, {"error": str(e)})
        except ValueError as e:
            return self._json(400, {"error": str(e)})
        self._json(200, result)

    @staticmethod
    def _motor(body: dict) -> int:
        motor_id = body.get("motor")
        if isinstance(motor_id, bool) or motor_id not in MOTOR_IDS:
            raise ValueError(f"motor must be one of {list(MOTOR_IDS)}")
        return motor_id

    @staticmethod
    def _notes(body: dict) -> str:
        return str(body.get("notes", ""))[:500].replace("\n", " ")


def main():
    global panel
    parser = argparse.ArgumentParser(description="Motor bench control panel (http://127.0.0.1:PORT)")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--no-browser", action="store_true", help="don't open a browser tab")
    args = parser.parse_args()

    panel = Panel()
    try:
        panel.start()
    except Exception as e:
        print(f"✗ Could not connect to the MQTT broker at {BROKER_IP}:{BROKER_PORT}: {e}")
        sys.exit(1)

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    httpd.daemon_threads = True
    url = f"http://127.0.0.1:{args.port}"
    log(f"Control panel running at {url}  (Ctrl-C to stop; motors are set to 0 on exit)")
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print()
        log("Shutting down...")
    finally:
        panel.shutdown()
        httpd.server_close()


if __name__ == "__main__":
    main()
