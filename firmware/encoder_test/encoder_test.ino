// ============================================================================
// FSAE EV26 TEST BENCH - ENCODER PIN TEST (Teensy 4.1, any corner)
// ----------------------------------------------------------------------------
// Standalone diagnostic for a stuck encoder channel. No Ethernet, no MQTT.
// Flash it in place of motor_node_<corner> and open the Serial Monitor (115200).
//
// Every 0.5 s, for encoder A (pin 18) and B (pin 17):
//   - edges counted by interrupt (what the Encoder library would see),
//   - the digital level right now,
//   - the analog min/max voltage on the same pin, and how many times the
//     analog voltage went low.
//   Spare pins 14, 15, 16 and 19 are watched for edges too (internal pull-up
//   on), so a wire moved to one of them can be tested before changing firmware.
//   While the shaft turns, analog vs digital separates two faults:
//     analog stays high  -> the signal never reaches the pin (wiring/contact)
//     analog toggles but digital doesn't -> the pin's digital input is damaged
//
// Every 10 s, while the shaft is still, a pin check: each encoder pin is read
// with the Teensy's internal pull-down, then pull-up. With the ~1 kOhm external
// pull-up connected, both read ~3.3 V. A pin that follows the internal pull
// (~0 V with pull-down) has nothing outside connected to it: a bad solder
// joint or contact.
//
// Motor: stays at zero throttle unless you type  r  (or  r 0.6, or  r 0.6 60)
// in the Serial Monitor. Then it ramps forward at 0.25/s to that effort and
// holds until you type anything, or for the given seconds (default 20, at most
// 120) counted from the start, then ramps down. Closing the Serial Monitor
// also ramps down. The HV killswitch is the emergency stop. Turning the shaft
// slowly by hand (HV off) works too.
//
// While this is on a board, that corner is offline on MQTT and find_boards.py
// reports it as running "EncoderTest". Reflash motor_node_<corner> to go back.
// ============================================================================
#include <IntervalTimer.h>

// ---- Pins (same as motor_node) ---------------------------------------------
const uint8_t PWM_PIN = 23;              // throttle PWM to the motor controller
const uint8_t DIRECTION_PIN = 22;        // open-drain: released = forward
const uint8_t ENC_A_PIN = 18;
const uint8_t ENC_B_PIN = 17;

// Watched pins: the two encoder pins (with analog), then spares (edges only)
const uint8_t WATCH_PINS[] = {ENC_A_PIN, ENC_B_PIN, 14, 15, 16, 19};
const int N_WATCH = sizeof(WATCH_PINS) / sizeof(WATCH_PINS[0]);
const int N_ENC = 2;                     // first two entries are A and B
const char* ENC_NAME[N_ENC] = {"A", "B"};

// ---- Test configuration ----------------------------------------------------
const float DEFAULT_RUN_EFFORT = 0.5f;   // LF starts at ~0.42
const float RAMP_PER_S = 0.25f;          // the controller won't start from a hard step
const float DEFAULT_RUN_S = 20.0f;
const float MAX_RUN_S = 120.0f;
const uint32_t STATUS_PERIOD_MS = 500;
const uint32_t CHECK_PERIOD_MS = 10000;

const float ADC_VREF = 3.3f;
const float ADC_MAX  = 4095.0f;          // 12-bit
const float LOW_V  = 1.0f;               // analog "low" / "high" thresholds, with hysteresis
const float HIGH_V = 2.3f;

inline float toVolts(float raw) { return raw * ADC_VREF / ADC_MAX; }

// ============================================================================
// MOTOR OUTPUT - 1 kHz ramp, same as the sensor test
// ============================================================================
IntervalTimer controlTimer;
volatile float targetEffort = 0.0f;      // [0, 1], forward only
volatile float currentEffort = 0.0f;

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

// ============================================================================
// EDGE COUNTING (one ISR per watched pin)
// ============================================================================
volatile uint32_t edges[N_WATCH];
volatile bool countEdges = true;         // off during the pin check

void edge0() { if (countEdges) edges[0]++; }
void edge1() { if (countEdges) edges[1]++; }
void edge2() { if (countEdges) edges[2]++; }
void edge3() { if (countEdges) edges[3]++; }
void edge4() { if (countEdges) edges[4]++; }
void edge5() { if (countEdges) edges[5]++; }
void (*const EDGE_ISR[N_WATCH])() = {edge0, edge1, edge2, edge3, edge4, edge5};

// ============================================================================
// ANALOG WATCH on A and B (sampled continuously from loop)
// ============================================================================
struct AnalogWatch {
  int lo = 1 << 30, hi = -1;
  uint32_t lows = 0;                     // high -> low transitions
  bool isLow = false;
  void add(int raw) {
    if (raw < lo) lo = raw;
    if (raw > hi) hi = raw;
    float v = toVolts(raw);
    if (!isLow && v < LOW_V) { isLow = true; lows++; }
    else if (isLow && v > HIGH_V) { isLow = false; }
  }
  void reset() { lo = 1 << 30; hi = -1; lows = 0; }
};
AnalogWatch aw[N_ENC];

float meanVolts(uint8_t pin, int n) {
  analogRead(pin);
  double sum = 0;
  for (int i = 0; i < n; i++) sum += analogRead(pin);
  return toVolts((float)(sum / n));
}

// ============================================================================
// PIN CHECK - is anything outside connected to each encoder pin?
// ============================================================================
void pinCheck() {
  Serial.println("\n--- pin check (shaft still; internal pull-down vs pull-up) ---");
  countEdges = false;
  for (int i = 0; i < N_ENC; i++) {
    uint8_t pin = WATCH_PINS[i];
    pinMode(pin, INPUT_PULLDOWN);        // ~100k to GND
    delay(2);
    float vd = meanVolts(pin, 64);
    pinMode(pin, INPUT_PULLUP);          // ~22k to 3.3 V (what the Encoder library uses)
    delay(2);
    float vu = meanVolts(pin, 64);
    Serial.printf("  %s (pin %d): with pull-down %.2f V, with pull-up %.2f V  => ",
                  ENC_NAME[i], pin, vd, vu);
    if (vu < 0.5f) {
      Serial.println("held LOW. The encoder output is on at this shaft position (normal),");
      Serial.println("     or the pin is shorted to GND. Turn the shaft a little and wait for the next check.");
    } else if (vd > 2.5f) {
      Serial.println("external pull-up connected (it beats the internal pull-down).");
    } else if (vd < 0.5f && vu > 2.8f) {
      Serial.printf("FLOATING: nothing outside reaches pin %d. The pull-up and the encoder wire\n", pin);
      Serial.println("     aren't connected to the pin: check the header solder joint / socket contact.");
    } else {
      Serial.printf("in between: a resistive connection to the pull-up (bad contact).\n");
    }
  }
  delay(2);
  countEdges = true;
  Serial.println("  type  r  to spin the motor (r 0.6 for another effort), or turn the shaft by hand\n");
}

// ============================================================================
// STATUS + DIAGNOSIS
// ============================================================================
void printStatus(uint32_t windowMs) {
  uint32_t e[N_WATCH];
  noInterrupts();
  for (int i = 0; i < N_WATCH; i++) { e[i] = edges[i]; edges[i] = 0; }
  interrupts();

  for (int i = 0; i < N_ENC; i++) {
    uint8_t pin = WATCH_PINS[i];
    Serial.printf("%s(pin %d): level %d, edges %6lu, analog %.2f-%.2f V, lows %5lu | ",
                  ENC_NAME[i], pin, digitalRead(pin), (unsigned long)e[i],
                  aw[i].hi < 0 ? NAN : toVolts(aw[i].lo), aw[i].hi < 0 ? NAN : toVolts(aw[i].hi),
                  (unsigned long)aw[i].lows);
  }
  Serial.print("spare edges");
  for (int i = N_ENC; i < N_WATCH; i++) Serial.printf(" %d:%lu", WATCH_PINS[i], (unsigned long)e[i]);
  Serial.printf(" | effort %.2f\n", (float)currentEffort);

  // Diagnosis when one channel moves and the other doesn't
  static uint32_t lastDiagMs = 0;
  if (millis() - lastDiagMs >= 2000) {
    for (int i = 0; i < N_ENC; i++) {
      int other = 1 - i;
      if (e[other] < 20 || e[i] > 0) continue;
      lastDiagMs = millis();
      uint8_t pin = WATCH_PINS[i];
      float lo = toVolts(aw[i].lo);
      Serial.printf("  >> %s (pin %d) is stuck while %s turns: ", ENC_NAME[i], pin, ENC_NAME[other]);
      if (aw[i].lows > 0) {
        Serial.printf("the analog voltage DOES toggle (%lu lows) but the digital input never\n",
                      (unsigned long)aw[i].lows);
        Serial.printf("     changes, so pin %d's digital input is damaged. Move %s to a spare pin.\n",
                      pin, ENC_NAME[i]);
      } else if (lo > HIGH_V) {
        Serial.printf("the pin never goes below %.2f V, so the encoder's signal isn't reaching\n", lo);
        Serial.println("     the pin. See the pin check: FLOATING = bad joint/contact at the pin;");
        Serial.println("     pull-up connected = the encoder wire isn't reaching that node, or the node is");
        Serial.println("     tied straight to 3.3 V (a shorted pull-up resistor).");
      } else {
        Serial.printf("it only dips to %.2f V, not low enough for a logic 0: a resistive\n", lo);
        Serial.println("     connection between the encoder wire and the pin.");
      }
    }
  }
  for (int i = 0; i < N_ENC; i++) aw[i].reset();
}

// ============================================================================
// SERIAL COMMANDS
// ============================================================================
bool running = false;                    // motor run requested or ramping down
uint32_t runStartMs = 0;
uint32_t runMs = 0;
char lineBuf[32];
uint8_t lineLen = 0;
uint32_t lastCharMs = 0;

void stopRun(const char* why) {
  if (targetEffort > 0.0f) Serial.printf("! stopping: %s - ramping down\n", why);
  targetEffort = 0.0f;
}

void processLine() {
  lineBuf[lineLen] = '\0';
  char* s = lineBuf;
  while (*s == ' ') s++;
  lineLen = 0;
  if (tolower(*s) != 'r') {
    if (*s) Serial.println("commands:  r  spin at 0.5 for 20 s   r 0.6  at effort 0.6   r 0.6 60  for 60 s");
    return;
  }
  float e = DEFAULT_RUN_EFFORT;
  float secs = DEFAULT_RUN_S;
  char* arg = s + 1;
  while (*arg == ' ') arg++;
  if (*arg) {
    char* end;
    e = strtof(arg, &end);
    if (end == arg || !(e > 0.0f && e <= 1.0f)) {
      Serial.println("! effort must be a number between 0 and 1, e.g.  r 0.6");
      return;
    }
    arg = end;
    while (*arg == ' ') arg++;
    if (*arg) {
      secs = strtof(arg, &end);
      if (end == arg || !(secs > 0.0f && secs <= MAX_RUN_S)) {
        Serial.printf("! seconds must be between 0 and %.0f, e.g.  r 0.6 60\n", MAX_RUN_S);
        return;
      }
    }
  }
  Serial.printf("\n=== spinning forward at effort %.2f for %.0f s (ramp %.2f/s). Type anything to stop. ===\n",
                e, secs, RAMP_PER_S);
  targetEffort = e;
  running = true;
  runStartMs = millis();
  runMs = (uint32_t)(secs * 1000.0f);
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    lastCharMs = millis();
    if (running) {                       // any input during a run stops it
      if (c != '\n' && c != '\r') stopRun("input received");
      continue;
    }
    if (c == '\n' || c == '\r') processLine();
    else if (lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
  }
  // Serial Monitor set to "No line ending": take the line after a short pause
  if (lineLen && !running && millis() - lastCharMs > 100) processLine();
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
  analogReadAveraging(1);                // fast, to catch short low pulses

  for (int i = 0; i < N_WATCH; i++) {
    pinMode(WATCH_PINS[i], INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(WATCH_PINS[i]), EDGE_ISR[i], CHANGE);
  }

  controlTimer.priority(192);
  controlTimer.begin(controlISR, 1000);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {}
  Serial.println("\n=== encoder pin test (motor_node is NOT running: this corner is offline on MQTT) ===");
}

void loop() {
  static uint32_t lastStatusMs = 0, lastCheckMs = 0;
  static bool checkedOnce = false;

  handleSerial();

  // Motor run bookkeeping
  if (running) {
    if (targetEffort > 0.0f && millis() - runStartMs > runMs) stopRun("run time reached");
    if (targetEffort > 0.0f && !Serial) stopRun("Serial Monitor closed");
    if (targetEffort == 0.0f && currentEffort == 0.0f) {
      running = false;
      Serial.println("=== motor stopped ===\n");
    }
  }

  // Continuous analog watch on A and B
  for (int i = 0; i < N_ENC; i++) aw[i].add(analogRead(WATCH_PINS[i]));

  uint32_t now = millis();
  if (now - lastStatusMs >= STATUS_PERIOD_MS) {
    uint32_t window = now - lastStatusMs;
    lastStatusMs = now;
    bool still = (edges[0] == 0 && edges[1] == 0);
    if (Serial) printStatus(window);
    else for (int i = 0; i < N_ENC; i++) aw[i].reset();

    // Pin check: first chance after boot, then every 10 s, only with the shaft
    // still and the motor off (moving edges would confuse the readings)
    if (Serial && still && !running && currentEffort == 0.0f &&
        (!checkedOnce || now - lastCheckMs >= CHECK_PERIOD_MS)) {
      pinCheck();
      checkedOnce = true;
      lastCheckMs = millis();
      lastStatusMs = millis();
    }
  }
}
