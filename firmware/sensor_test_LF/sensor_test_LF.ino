// ============================================================================
// FSAE EV26 TEST BENCH - CURRENT SENSOR TEST for LF (Teensy 4.1)
// ----------------------------------------------------------------------------
// Standalone diagnostic: no Ethernet, no MQTT, no PC software. Flash it onto
// the LF board in place of motor_node_LF and open the Serial Monitor (115200).
//
// 1. Sensor check (automatic, motor stays off). Every 10 s it measures pin 21
//    with the pin's internal pull-up and then pull-down switched in. A pin the
//    sensor is really driving barely moves; a pin nothing is connected to
//    follows the pull resistor. It also scans the other free analog pins for
//    anything being driven, in case the AOUT wire landed on a different pin.
//    Between checks it prints pin 21's voltage once a second.
//
// 2. Motor run (only when you type it in the Serial Monitor):
//      r        run at DEFAULT_RUN_EFFORT
//      r 0.7    run at effort 0.7 (forward only, 0 < effort <= 1)
//    It records the zero-current reading with the motor stopped, ramps LF
//    forward at 0.25 effort/s, holds HOLD_S seconds, ramps back to 0, then
//    prints how much the sensor output moved. Typing anything during the run,
//    or closing the Serial Monitor, ramps the motor back down. The HV
//    killswitch is still the emergency stop.
//      c        run the sensor check now
//
// While this is on the board, LF is offline on MQTT and find_boards.py reports
// LF as running "SensorTest_LF". Reflash firmware/motor_node_LF to go back.
// ============================================================================
#include <Encoder.h>
#include <IntervalTimer.h>

// ---- Board / test configuration --------------------------------------------
#define CORNER "LF"
const long ENCODER_SIGN = -1;            // LF, verified 2026-11-02
const uint8_t SENSOR_PIN = 21;           // current sensor AOUT (A7), same as motor_node
const float SENSOR_V_PER_A = 0.0f;       // sensitivity from the datasheet, in V per A.
                                         // 0 = unknown: results are shown in volts only.
const float DEFAULT_RUN_EFFORT = 0.6f;   // ~1400 rpm free-spinning; motor starts at ~0.42
const float START_THRESHOLD = 0.42f;     // measured on LF 2026-11-02
const float RAMP_PER_S = 0.25f;          // the controller won't start from a hard step
const float BASELINE_S = 1.0f;           // zero-current reading before the motor moves
const float HOLD_S     = 5.0f;
const float SETTLE_S   = 1.5f;           // keep recording after effort is back at 0

// ---- Pins (same as motor_node) ---------------------------------------------
const uint8_t PWM_PIN = 23;              // throttle PWM to the motor controller
const uint8_t DIRECTION_PIN = 22;        // open-drain: released = forward
const uint8_t ENC_A_PIN = 18;
const uint8_t ENC_B_PIN = 17;
// Free analog pins to scan for a driven signal. Skips the encoder (17, 18),
// the motor outputs (22, 23) and the sensor pin itself.
const uint8_t SCAN_PINS[] = {14, 15, 16, 19, 20, 24, 25, 26, 27, 38, 39, 40, 41};

const float ADC_VREF = 3.3f;
const float ADC_MAX  = 4095.0f;          // 12-bit
const float COUNTS_PER_REV = 2400.0f;    // 600 P/R, 4x decoding
const uint32_t ROW_PERIOD_MS = 50;       // 20 rows/s during a run
const uint32_t IDLE_PRINT_MS = 1000;
const uint32_t CHECK_PERIOD_MS = 10000;

inline float toVolts(float raw) { return raw * ADC_VREF / ADC_MAX; }

// ============================================================================
// MOTOR OUTPUT - 1 kHz timer ramps currentEffort toward targetEffort and
// writes the PWM, so the ramp keeps going even if Serial printing stalls.
// ============================================================================
IntervalTimer controlTimer;
volatile float targetEffort = 0.0f;      // [0, 1], forward only
volatile float currentEffort = 0.0f;     // what's on the PWM pin right now

void controlISR() {
  static int lastPwm = -1;
  float cur = currentEffort;
  float tgt = targetEffort;
  const float step = RAMP_PER_S * 0.001f;
  if (cur < tgt)      cur = fminf(cur + step, tgt);
  else if (cur > tgt) cur = fmaxf(cur - step, tgt);
  currentEffort = cur;
  int pwm = (int)(cur * 255.0f + 0.5f);
  if (pwm != lastPwm) {
    analogWrite(PWM_PIN, pwm);
    lastPwm = pwm;
  }
}

Encoder wheelEncoder(ENC_A_PIN, ENC_B_PIN);

// ============================================================================
// ADC STATS
// ============================================================================
struct Stats {
  uint32_t n = 0;
  double sum = 0;
  int lo = 1 << 30;
  int hi = -1;
  void add(int v) { n++; sum += v; if (v < lo) lo = v; if (v > hi) hi = v; }
  float meanV() const { return n ? toVolts((float)(sum / n)) : NAN; }
  float loV() const { return toVolts(lo); }
  float hiV() const { return toVolts(hi); }
  float rippleV() const { return n ? toVolts(hi - lo) : NAN; }
  void reset() { *this = Stats(); }
};

float readPinV(uint8_t pin, uint8_t mode, int n) {
  pinMode(pin, mode);
  delayMicroseconds(500);
  analogRead(pin);                       // discard the first conversion
  double sum = 0;
  for (int i = 0; i < n; i++) sum += analogRead(pin);
  return toVolts((float)(sum / n));
}

// ============================================================================
// SENSOR CHECK (motor off)
// ============================================================================
float bootV = NAN;                       // pin 21 at power-up, before any pinMode

void sensorCheck() {
  float v  = readPinV(SENSOR_PIN, INPUT_DISABLE, 400);
  float vu = readPinV(SENSOR_PIN, INPUT_PULLUP, 400);    // ~22k to 3.3 V
  float vd = readPinV(SENSOR_PIN, INPUT_PULLDOWN, 400);  // ~100k to GND
  pinMode(SENSOR_PIN, INPUT_DISABLE);
  float shift = vu - vd;

  Serial.printf("\n--- sensor check, pin %d (motor off) ---\n", SENSOR_PIN);
  Serial.printf("  at power-up (what motor_node reads): %.3f V\n", bootV);
  Serial.printf("  now: %.3f V   with 22k pull-up: %.3f V   with 100k pull-down: %.3f V\n",
                v, vu, vd);
  if (shift > 1.0f) {
    Serial.println("  => FLOATING: nothing is driving pin 21. The AOUT wire isn't connected");
    Serial.println("     to pin 21 (wrong pin, loose jumper, or bad contact).");
  } else if (shift > 0.05f) {
    float rs = (3.3f - vu > 0.05f) ? 22.0f * (vu - v) / (3.3f - vu) : NAN;
    Serial.printf("  => WEAKLY driven: whatever feeds pin 21 is high impedance (~%.0f kOhm).\n", rs);
    Serial.println("     A hall sensor output should be stiff; check for a series resistor or bad contact.");
  } else {
    Serial.printf("  => driven: the pull resistors moved it only %.0f mV.\n", shift * 1000.0f);
    if (v > 3.15f) {
      Serial.println("     But it's at the 3.3 V rail, which isn't a zero-current output. Pin 21 is");
      Serial.println("     connected to 3.3 V (wires swapped, solder bridge), or the sensor is stuck high.");
    } else if (v < 0.10f) {
      Serial.println("     But it's at GND. Pin 21 is shorted to GND, or the sensor is stuck low.");
    } else {
      Serial.println("     Looks like a real sensor output. If your meter disagrees, measure with the");
      Serial.println("     black probe on Teensy GND and the red probe on pin 21 itself.");
    }
  }

  Serial.print("  other analog pins being driven: ");
  int found = 0;
  for (uint8_t pin : SCAN_PINS) {
    float pu = readPinV(pin, INPUT_PULLUP, 50);
    float pd = readPinV(pin, INPUT_PULLDOWN, 50);
    pinMode(pin, INPUT_DISABLE);
    if (pu - pd < 0.10f) {
      float pv = readPinV(pin, INPUT_DISABLE, 50);
      Serial.printf("%spin %d = %.2f V%s", found ? ", " : "", pin, pv,
                    (pv > 1.4f && pv < 2.0f) ? " (<- near 1.7 V)" : "");
      found++;
    }
  }
  Serial.println(found ? "" : "none");
  Serial.println("  type  r  to run the motor and test the sensor (r 0.7 for another effort), c to re-check\n");
}

// ============================================================================
// MOTOR RUN
// ============================================================================
enum Phase { IDLE, BASELINE, RAMP_UP, HOLD, RAMP_DOWN, SETTLE };
const char* PHASE_NAME[] = {"idle", "baseline", "ramp-up", "hold", "ramp-down", "settle"};
Phase phase = IDLE;

float runEffort = DEFAULT_RUN_EFFORT;
bool aborted = false;
uint32_t runStartMs = 0, phaseStartMs = 0;
float baselineV = NAN;
Stats win, baselineStats, holdStats;
float maxDeltaV = 0.0f, maxDeltaT = 0.0f, maxDeltaEffort = 0.0f;
double holdRpmSum = 0.0; uint32_t holdRpmN = 0;
int32_t lastCount = 0; uint32_t lastRowUs = 0;

// int, not Phase: Arduino's generated prototypes go above the enum
void setPhase(int p) { phase = (Phase)p; phaseStartMs = millis(); }
float phaseSeconds() { return (millis() - phaseStartMs) * 1e-3f; }

void startRun(float effort) {
  runEffort = effort;
  aborted = false;
  baselineV = NAN;
  baselineStats.reset(); holdStats.reset(); win.reset();
  maxDeltaV = maxDeltaT = maxDeltaEffort = 0.0f;
  holdRpmSum = 0.0; holdRpmN = 0;
  lastCount = ENCODER_SIGN * wheelEncoder.read();
  lastRowUs = micros();
  runStartMs = millis();
  setPhase(BASELINE);
  Serial.printf("\n=== motor run: effort %.2f forward, ramp %.2f/s, hold %.1f s ===\n",
                runEffort, RAMP_PER_S, HOLD_S);
  if (runEffort < START_THRESHOLD) {
    Serial.printf("(effort is below ~%.2f, where LF starts; the motor may not turn)\n", START_THRESHOLD);
  }
  Serial.println("type anything to stop. Measuring zero-current reading with the motor stopped...");
  Serial.printf("%7s %-9s %6s %6s %8s %9s %9s%s\n", "t_s", "phase", "effort", "rpm",
                "pin21_V", "change_V", "ripple_V", SENSOR_V_PER_A > 0 ? "  current_A" : "");
}

void abortRun(const char* why) {
  targetEffort = 0.0f;
  if (phase != BASELINE && phase != RAMP_UP && phase != HOLD) return;  // already stopping
  Serial.printf("! stopping: %s - ramping down\n", why);
  aborted = true;
  setPhase(phase == BASELINE ? SETTLE : RAMP_DOWN);  // from BASELINE the motor never moved
}

void printRow() {
  uint32_t now = micros();
  int32_t count = ENCODER_SIGN * wheelEncoder.read();
  float dt = (now - lastRowUs) * 1e-6f;
  float rpm = dt > 0 ? (count - lastCount) / COUNTS_PER_REV * 60.0f / dt : 0.0f;
  lastCount = count;
  lastRowUs = now;

  float v = win.meanV();
  float eff = currentEffort;
  float t = (millis() - runStartMs) * 1e-3f;
  float delta = isnan(baselineV) ? NAN : v - baselineV;
  if (!isnan(delta) && fabsf(delta) > fabsf(maxDeltaV)) {
    maxDeltaV = delta; maxDeltaT = t; maxDeltaEffort = eff;
  }
  if (phase == HOLD) { holdRpmSum += rpm; holdRpmN++; }

  Serial.printf("%7.2f %-9s %6.3f %6.0f %8.3f %+9.3f %9.3f", t, PHASE_NAME[phase], eff, rpm,
                v, delta, win.rippleV());
  if (SENSOR_V_PER_A > 0 && !isnan(delta)) Serial.printf("  %+9.2f", delta / SENSOR_V_PER_A);
  Serial.println();
  win.reset();
}

void printSummary() {
  Serial.printf("\n=== run summary (%s) ===\n", aborted ? "stopped early" : "complete");
  Serial.printf("effort %.2f, zero-current reading %.3f V\n", runEffort, baselineV);
  float holdRpm = holdRpmN ? (float)(holdRpmSum / holdRpmN) : NAN;
  if (holdStats.n) {
    float hv = holdStats.meanV();
    Serial.printf("during hold: %.3f V average (%.3f to %.3f), change %+.3f V, speed %.0f rpm\n",
                  hv, holdStats.loV(), holdStats.hiV(), hv - baselineV, holdRpm);
    if (SENSOR_V_PER_A > 0) Serial.printf("  = %+.2f A\n", (hv - baselineV) / SENSOR_V_PER_A);
  }
  Serial.printf("largest change (50 ms average): %+.3f V at t=%.2f s, effort %.2f\n",
                maxDeltaV, maxDeltaT, maxDeltaEffort);

  Serial.print("verdict: ");
  if (isnan(baselineV)) {
    Serial.println("stopped before the zero-current reading finished; nothing measured.");
  } else if (baselineV > 3.15f || baselineV < 0.10f) {
    Serial.println("the sensor output sits at a rail, so it can't show current.");
    Serial.println("         Fix the wiring first (see the sensor check).");
  } else if (holdStats.n && fabsf(holdRpm) < 50.0f) {
    Serial.println("the motor didn't spin (HV killswitch off?), so no current flowed");
    Serial.println("         and this run doesn't test the sensor.");
  } else if (fabsf(maxDeltaV) < 0.010f) {
    Serial.println("no measurable change (under 10 mV). Either the sensor isn't on a wire that");
    Serial.println("         carries LF's current, or free-spinning current is too small to see:");
    Serial.println("         try a higher effort (r 0.9).");
  } else {
    Serial.printf("the sensor responded to the motor current%s.\n",
                  maxDeltaV < 0 ? " (output went DOWN: current flows the other way through it)" : "");
  }
  Serial.println("type  r  to run again, c to re-check the sensor\n");
}

void runStep() {
  if (!Serial) abortRun("Serial Monitor closed");

  switch (phase) {
    case BASELINE:
      if (phaseSeconds() >= BASELINE_S) {
        baselineV = baselineStats.meanV();
        Serial.printf("  zero-current reading: %.3f V (ripple %.3f V). Ramping up...\n",
                      baselineV, baselineStats.rippleV());
        targetEffort = runEffort;
        setPhase(RAMP_UP);
      }
      break;
    case RAMP_UP:
      if (currentEffort >= runEffort - 1e-4f) setPhase(HOLD);
      break;
    case HOLD:
      if (phaseSeconds() >= HOLD_S) {
        targetEffort = 0.0f;
        setPhase(RAMP_DOWN);
      }
      break;
    case RAMP_DOWN:
      if (currentEffort == 0.0f) setPhase(SETTLE);
      break;
    case SETTLE:
      if (phaseSeconds() >= SETTLE_S) {
        printSummary();
        setPhase(IDLE);
      }
      break;
    default:
      break;
  }
}

// ============================================================================
// SERIAL COMMANDS
// ============================================================================
char lineBuf[32];
uint8_t lineLen = 0;
uint32_t lastCharMs = 0;
bool checkRequested = false;

void processLine() {
  lineBuf[lineLen] = '\0';
  char* s = lineBuf;
  while (*s == ' ') s++;
  lineLen = 0;
  if (*s == '\0') return;

  char cmd = tolower(*s);
  if (cmd == 'r') {
    float e = DEFAULT_RUN_EFFORT;
    char* arg = s + 1;
    while (*arg == ' ') arg++;
    if (*arg) {
      char* end;
      e = strtof(arg, &end);
      if (end == arg || !(e > 0.0f && e <= 1.0f)) {
        Serial.println("! effort must be a number between 0 and 1, e.g.  r 0.7");
        return;
      }
    }
    startRun(e);
  } else if (cmd == 'c') {
    checkRequested = true;
  } else {
    Serial.println("commands:  r  run motor + test sensor   r 0.7  run at effort 0.7   c  sensor check");
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    lastCharMs = millis();
    if (phase != IDLE) {                 // any input during a run stops it
      if (c != '\n' && c != '\r') abortRun("input received");
      continue;
    }
    if (c == '\n' || c == '\r') {
      processLine();
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
  // Serial Monitor set to "No line ending": take the line after a short pause
  if (lineLen && phase == IDLE && millis() - lastCharMs > 100) processLine();
}

// ============================================================================
// SETUP / LOOP
// ============================================================================
void setup() {
  // Motor outputs first, so the controller sees "stopped" as early as possible
  analogWriteFrequency(PWM_PIN, 100000);
  analogWrite(PWM_PIN, 0);
  pinMode(DIRECTION_PIN, OUTPUT_OPENDRAIN);
  digitalWrite(DIRECTION_PIN, HIGH);     // released = forward

  analogReadResolution(12);
  analogReadAveraging(4);
  double sum = 0;                        // pin 21 exactly as motor_node leaves it
  analogRead(SENSOR_PIN);
  for (int i = 0; i < 400; i++) sum += analogRead(SENSOR_PIN);
  bootV = toVolts((float)(sum / 400));
  pinMode(SENSOR_PIN, INPUT_DISABLE);

  controlTimer.priority(192);            // below the encoder pin interrupts
  controlTimer.begin(controlISR, 1000);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {}
  Serial.printf("\n=== %s current sensor test (motor_node is NOT running: LF is offline on MQTT) ===\n",
                CORNER);
  checkRequested = true;
}

void loop() {
  static uint32_t lastSampleUs = 0, lastRowMs = 0, lastIdleMs = 0, lastCheckMs = 0;

  handleSerial();

  uint32_t nowUs = micros();
  if (nowUs - lastSampleUs >= 1000) {    // 1 kHz sensor sampling
    lastSampleUs = nowUs;
    int raw = analogRead(SENSOR_PIN);
    win.add(raw);
    if (phase == BASELINE) baselineStats.add(raw);
    if (phase == HOLD) holdStats.add(raw);
  }

  uint32_t nowMs = millis();
  if (phase == IDLE) {
    if (checkRequested || nowMs - lastCheckMs >= CHECK_PERIOD_MS) {
      checkRequested = false;
      if (Serial) sensorCheck();
      lastCheckMs = lastIdleMs = millis();
      win.reset();
    } else if (nowMs - lastIdleMs >= IDLE_PRINT_MS) {
      lastIdleMs = nowMs;
      if (Serial) {
        int32_t count = ENCODER_SIGN * wheelEncoder.read();
        static int32_t idleLastCount = count;
        float rpm = (count - idleLastCount) / COUNTS_PER_REV * 60.0f / (IDLE_PRINT_MS * 1e-3f);
        idleLastCount = count;
        Serial.printf("pin %d: %.3f V (ripple %.3f V)   rpm %.0f\n", SENSOR_PIN, win.meanV(),
                      win.rippleV(), rpm);
      }
      win.reset();
    }
  } else {
    runStep();
    if (phase != IDLE && nowMs - lastRowMs >= ROW_PERIOD_MS) {
      lastRowMs = nowMs;
      printRow();
    }
  }
}
