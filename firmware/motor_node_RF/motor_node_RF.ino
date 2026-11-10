// ****************************************************************************
// GENERATED FILE - DO NOT EDIT. Edit firmware/motor_node/motor_node.ino, then
// run `python3 firmware/make_corner_sketches.py` to regenerate all 4 corners.
// ****************************************************************************
// ============================================================================
// FSAE EV26 TEST BENCH - MOTOR NODE (Teensy 4.1)
// ----------------------------------------------------------------------------
// One of these runs per motor. It:
//   - receives an effort command [-1, +1] over MQTT and drives the motor
//     controller with PWM (throttle) + an open-drain direction line
//   - samples the shaft encoder at 1 kHz and publishes the samples over MQTT
//     in batches (see STATE PACKET below)
//
// Timing: a 1 kHz hardware timer interrupt (sampleISR) does the control update
// (watchdog, ramp, PWM) and takes the encoder sample. The main loop handles
// networking and samples the current/voltage sensor pins, so control and
// encoder sampling keep running even while the network is busy or reconnecting.
//
// Each board is named after its wheel corner (LF, RF, LR, RR).
// ============================================================================
#include <NativeEthernet.h>
#include <PubSubClient.h>
#include <TimerEvent.h>
#include <Encoder.h>
#include <IntervalTimer.h>

// ============================================================================
// PER-BOARD CONFIGURATION
// ============================================================================
// Values come from CORNERS in firmware/make_corner_sketches.py.
#define MOTOR_NAME "RF"            // wheel corner: sets the MQTT topics and client ID
const uint8_t MAC_LAST_BYTE = 2;      // unique per board
const long ENCODER_SIGN = -1;         // NOT VERIFIED - check positive effort reads positive rpm, flip if not

const char* stateTopic   = "robot/motor/" MOTOR_NAME "/state";
const char* commandTopic = "robot/motor/" MOTOR_NAME "/command";
const char* mqttClientId = "Teensy_Motor_" MOTOR_NAME;

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, MAC_LAST_BYTE};  // unique per board
IPAddress brokerIP(192, 168, 10, 1);
const uint16_t brokerPort = 1883;

// ============================================================================
// DEFINE I/O PINS
// ============================================================================
const uint8_t PWM_PIN = 23;              // PWM "throttle" output to motor controller
const uint8_t DIRECTION_PIN = 22;        // Open-drain: released = forward, pulled to GND = reverse
const uint8_t CURRENT_SENSOR_PIN = 21;   // Analog: current sensor output (LF: WCS1600 board)
const uint8_t VOLTAGE_SENSOR_PIN = 20;   // Analog: bus voltage sensor (not wired up yet)
const uint8_t WHEEL_ENCODER_A_PIN = 18;
const uint8_t WHEEL_ENCODER_B_PIN = 17;

// ============================================================================
// ENCODER CONFIGURATION
// ----------------------------------------------------------------------------
// Taiss 600 P/R optical encoder (LPD3806-600BM style): NPN open collector,
// pulled up to 3.3 V with external resistors. Power the encoder from its own
// 9-24 V supply (ground shared with the Teensy), NOT the Teensy's VIN: VIN is
// only ~4.3 V on USB, and below 5 V the outputs stick low. The Encoder library counts every
// edge on A and B ("4x" decoding), so one shaft revolution = 4 * 600 counts.
// This unit is rated for 6400 rpm.
// ============================================================================
const float ENCODER_PPR    = 600.0f;
const float COUNTS_PER_REV = 4.0f * ENCODER_PPR;            // 2400
// ENCODER_SIGN (per board, set in the PER-BOARD CONFIGURATION block above):
// positive effort must read as positive velocity.

// ============================================================================
// DRIVE CONFIGURATION
// ----------------------------------------------------------------------------
// The controller won't start from a hard step, so effort is rate-limited.
// Rates are in effort-per-second; 0.25/s matches the old firmware
// (0.05 every 200 ms, i.e. 0 -> 1.0 in 4 s). Set a rate to e.g. 1000.0 to make
// that direction effectively a hard step.
// ============================================================================
const float RAMP_UP_PER_S   = 0.25f;   // when |effort| is increasing
const float RAMP_DOWN_PER_S = 0.25f;   // when |effort| is decreasing
const uint32_t COMMAND_TIMEOUT_MS = 1000;  // no command for this long -> target 0
const uint8_t PWM_MAX = 255;           // 8-bit analogWrite

// ============================================================================
// ANALOG SENSOR CONFIGURATION
// ----------------------------------------------------------------------------
// The current/voltage sensor pins are read at ~1 kHz in loop() and averaged
// over each packet. The packet carries the raw pin voltages; the PC converts
// them to amps and volts (utils/sensors.py), so calibrating needs no reflash.
// Teensy 4.1 analog pins take 0-3.3 V only.
// ============================================================================
const uint8_t  ADC_BITS = 12;
const float    ADC_VREF = 3.3f;
const float    ADC_MAX  = 4095.0f;          // (1 << ADC_BITS) - 1
const uint32_t ANALOG_PERIOD_US = 1000;

// ============================================================================
// SAMPLING CONFIGURATION
// ============================================================================
const int      SAMPLE_PERIOD_US   = 1000;  // 1 kHz control update + encoder sample
const uint16_t SAMPLES_PER_PACKET = 50;    // -> 20 MQTT packets/s
const uint32_t RING_SIZE          = 4096;  // ~4 s of buffering if the network stalls
static_assert((RING_SIZE & (RING_SIZE - 1)) == 0, "RING_SIZE must be a power of two");

// ============================================================================
// STATE PACKET (version 3) - published on stateTopic, little-endian, packed
// ----------------------------------------------------------------------------
//   PacketHeader (20 bytes) followed by sampleCount x Sample (12 bytes each)
//   Python: header '<HHIIff', then each sample '<Iif'
// ============================================================================
const uint16_t PACKET_VERSION = 3;

struct __attribute__((packed)) PacketHeader {
  uint16_t version;         // PACKET_VERSION
  uint16_t sampleCount;     // number of samples that follow
  uint32_t sequence;        // packet counter since boot; a gap = MQTT lost a packet
  uint32_t droppedSamples;  // samples lost on the Teensy (buffer overflow), cumulative since boot
  float currentSensorV;     // current sensor pin voltage, averaged over this packet (V)
  float voltageSensorV;     // voltage sensor pin voltage, averaged over this packet (V)
};
static_assert(sizeof(PacketHeader) == 20, "PacketHeader must be 20 bytes");

struct __attribute__((packed)) Sample {
  uint32_t timeStamp;       // micros() when sampled
  int32_t  encoderCount;    // raw count since boot, ENCODER_SIGN applied (COUNTS_PER_REV per rev)
  float    effort;          // effort being output at that instant [-1, +1]
};
static_assert(sizeof(Sample) == 12, "Sample must be 12 bytes");

// ============================================================================
// DRIVE STATE (shared between the MQTT callback, the main loop and the ISR)
// ============================================================================
volatile float targetEffort = 0.0f;          // latest commanded effort [-1, +1]
volatile float currentEffort = 0.0f;         // effort actually being output (ramped)
volatile uint32_t lastCommandMillis = 0;     // millis() of last valid command
volatile bool commandTimedOut = true;        // true until the first command arrives
volatile bool timeoutEvent = false;          // set by ISR, reported by loop

// ============================================================================
// SAMPLE BUFFER (single producer = ISR, single consumer = main loop)
// ============================================================================
Sample ring[RING_SIZE];
volatile uint32_t ringHead = 0;              // next slot the ISR writes (only ISR changes it)
volatile uint32_t ringTail = 0;              // next slot the loop reads (only loop changes it)
volatile uint32_t droppedSamples = 0;
uint32_t packetSequence = 0;
uint8_t packetBuf[sizeof(PacketHeader) + SAMPLES_PER_PACKET * sizeof(Sample)];

IntervalTimer sampleTimer;
Encoder wheelEncoder(WHEEL_ENCODER_A_PIN, WHEEL_ENCODER_B_PIN);

// Analog sensor averages (main loop only)
uint32_t analogSamples = 0;
uint32_t currentRawSum = 0;
uint32_t voltageRawSum = 0;
float currentSensorV = NAN;                  // latest packet-period averages, V at the pin
float voltageSensorV = NAN;                  // (NaN until the first average; the PC shows "no data")

// ============================================================================
// SLOW TIMERS (main loop)
// ============================================================================
const unsigned int debugTimerPeriod  = 500;    // ms - Serial Monitor printout
TimerEvent debugTimer;

// ============================================================================
// APPLY EFFORT - writes direction + PWM to the controller (called from ISR)
// ============================================================================
void applyEffort(float effort) {
  static int lastPwm = -1;
  static int lastDir = -1;

  // DIR only changes when effort is nonzero, so it never toggles while idle.
  // HIGH on an open-drain pin = released (forward); LOW = pulled to GND (reverse).
  int dir = lastDir;
  if (effort > 0.0f) {
    dir = HIGH;
  } else if (effort < 0.0f) {
    dir = LOW;
  }
  if (dir != lastDir && dir != -1) {
    digitalWrite(DIRECTION_PIN, dir);
    lastDir = dir;
  }

  float magnitude = fabsf(effort);
  if (magnitude > 1.0f) magnitude = 1.0f;
  int pwm = (int)(magnitude * PWM_MAX + 0.5f);
  if (pwm != lastPwm) {
    analogWrite(PWM_PIN, pwm);
    lastPwm = pwm;
  }
}

// ============================================================================
// RAMP - move currentEffort toward targetEffort at the configured rates
// ============================================================================
void rampTowardTarget(float dt) {
  float cur = currentEffort;
  float diff = targetEffort - cur;
  if (diff == 0.0f) return;

  // |effort| is increasing if we're starting from zero or moving away from it
  bool increasing = (cur == 0.0f) || ((cur > 0.0f) == (diff > 0.0f));
  float maxStep = (increasing ? RAMP_UP_PER_S : RAMP_DOWN_PER_S) * dt;
  float next = cur + constrain(diff, -maxStep, maxStep);

  // On a direction change, stop at exactly zero for one tick so the new
  // direction starts from zero at the ramp-up rate.
  if ((cur > 0.0f && next < 0.0f) || (cur < 0.0f && next > 0.0f)) {
    next = 0.0f;
  }
  currentEffort = next;
}

// ============================================================================
// 1 kHz ISR - control update, then encoder sample. Keep this short.
// ============================================================================
void sampleISR() {
  // Command watchdog: if the PC stops sending, ramp to zero
  if (!commandTimedOut && (millis() - lastCommandMillis > COMMAND_TIMEOUT_MS)) {
    commandTimedOut = true;
    targetEffort = 0.0f;
    timeoutEvent = true;
  }
  rampTowardTarget(SAMPLE_PERIOD_US * 1e-6f);
  applyEffort(currentEffort);

  // Sample into the ring buffer (drop + count if the loop hasn't kept up)
  uint32_t head = ringHead;
  if (head - ringTail >= RING_SIZE) {
    droppedSamples = droppedSamples + 1;
    return;
  }
  Sample &s = ring[head & (RING_SIZE - 1)];
  s.timeStamp = micros();
  s.encoderCount = ENCODER_SIGN * wheelEncoder.read();
  s.effort = currentEffort;
  __sync_synchronize();          // sample fully written before it's published
  ringHead = head + 1;
}

// ============================================================================
// MQTT MESSAGE CALLBACK (Effort Command)
// ============================================================================
void onMessageReceived(char* topic, byte* payload, unsigned int length) {
  // Verify we're receiving the correct topic
  if (strcmp(topic, commandTopic) != 0) {
    return;
  }

  // Expect exactly one little-endian float
  if (length != sizeof(float)) {
    return;
  }

  float effort = 0.0f;
  memcpy(&effort, payload, sizeof(float));
  if (isnan(effort) || isinf(effort)) {
    return;
  }

  noInterrupts();
  lastCommandMillis = millis();
  targetEffort = constrain(effort, -1.0f, 1.0f);
  bool wasTimedOut = commandTimedOut;
  commandTimedOut = false;
  interrupts();

  if (wasTimedOut) {
    Serial.println("> Receiving commands");
  }
}

// ============================================================================
// CREATE NETWORK OBJECTS
// ============================================================================
EthernetClient ethClient;
PubSubClient client(brokerIP, brokerPort, onMessageReceived, ethClient);

// ============================================================================
// PUBLISH - send every full batch of samples waiting in the ring buffer
// ============================================================================
void publishPendingSamples() {
  if (ringHead - ringTail >= SAMPLES_PER_PACKET) {
    takeAnalogAverages();
  }
  while (ringHead - ringTail >= SAMPLES_PER_PACKET) {
    __sync_synchronize();        // see the samples the ISR finished writing

    PacketHeader header;
    header.version = PACKET_VERSION;
    header.sampleCount = SAMPLES_PER_PACKET;
    header.sequence = packetSequence++;
    header.droppedSamples = droppedSamples;
    header.currentSensorV = currentSensorV;
    header.voltageSensorV = voltageSensorV;
    memcpy(packetBuf, &header, sizeof(header));

    uint32_t tail = ringTail;
    for (uint16_t i = 0; i < SAMPLES_PER_PACKET; i++) {
      memcpy(packetBuf + sizeof(header) + i * sizeof(Sample),
             &ring[(tail + i) & (RING_SIZE - 1)], sizeof(Sample));
    }
    __sync_synchronize();        // done reading before handing the slots back
    ringTail = tail + SAMPLES_PER_PACKET;

    // If this fails (e.g. disconnected) the packet is lost; the PC sees the
    // sequence gap.
    client.publish(stateTopic, packetBuf, sizeof(packetBuf), false);
  }
}

// ============================================================================
// ANALOG SENSORS - sampled from loop() at ~1 kHz, averaged per packet.
// (Not in the ISR: analogRead() calls yield() while it waits.)
// ============================================================================
void sampleAnalog() {
  static uint32_t lastUs = 0;
  uint32_t now = micros();
  if (now - lastUs < ANALOG_PERIOD_US) return;
  lastUs = now;
  currentRawSum += analogRead(CURRENT_SENSOR_PIN);
  voltageRawSum += analogRead(VOLTAGE_SENSOR_PIN);
  analogSamples++;
  if (analogSamples >= 1000) takeAnalogAverages();  // not publishing (MQTT down): keep sums small
}

// Average everything sampled since the last call. If nothing was sampled
// (loop blocked, e.g. reconnecting), the previous averages are kept.
void takeAnalogAverages() {
  if (analogSamples == 0) return;
  currentSensorV = (float)currentRawSum / analogSamples * ADC_VREF / ADC_MAX;
  voltageSensorV = (float)voltageRawSum / analogSamples * ADC_VREF / ADC_MAX;
  analogSamples = currentRawSum = voltageRawSum = 0;
}

// ============================================================================
// DEBUG TIMER CALLBACK - human-readable status on the Serial Monitor
// ============================================================================
void debugTimerCallback() {
  static int32_t lastCount = 0;
  static uint32_t lastMicros = 0;

  int32_t count = ENCODER_SIGN * wheelEncoder.read();
  uint32_t now = micros();
  float rpm = 0.0f;
  if (lastMicros != 0) {
    rpm = (count - lastCount) / COUNTS_PER_REV * 60.0f / ((now - lastMicros) * 1e-6f);
  }
  lastCount = count;
  lastMicros = now;

  if (!Serial) return;  // only print when the Serial Monitor is open
  Serial.printf("enc=%ld (A=%d B=%d) rev=%.2f rpm=%.0f | target=%+.2f out=%+.2f | "
                "isense=%.3fV vsense=%.3fV | mqtt=%s pkts=%lu dropped=%lu\n",
                (long)count,
                digitalRead(WHEEL_ENCODER_A_PIN), digitalRead(WHEEL_ENCODER_B_PIN),
                count / COUNTS_PER_REV, rpm,
                (float)targetEffort, (float)currentEffort,
                currentSensorV, voltageSensorV,
                client.connected() ? "up" : "DOWN",
                (unsigned long)packetSequence, (unsigned long)droppedSamples);
}

// ============================================================================
// CONNECT TO MQTT BROKER
// ============================================================================
void connectToBroker() {
  static unsigned long lastAttempt = 0;
  static uint8_t attemptCount = 0;

  // Throttle connection attempts (don't hammer the broker)
  if (lastAttempt != 0 && millis() - lastAttempt < 5000) {
    return;
  }
  lastAttempt = millis();

  Serial.print("Connecting to MQTT broker at ");
  Serial.print(brokerIP);
  Serial.print(":");
  Serial.println(brokerPort);

  if (client.connect(mqttClientId)) {
    Serial.println("✓ Connected to MQTT broker");
    attemptCount = 0;

    // Subscribe to effort command
    if (client.subscribe(commandTopic)) {
      Serial.print("✓ Subscribed to: ");
      Serial.println(commandTopic);
    } else {
      Serial.print("✗ Failed to subscribe to: ");
      Serial.println(commandTopic);
    }
  } else {
    attemptCount++;
    Serial.print("✗ Connection failed (attempt ");
    Serial.print(attemptCount);
    Serial.println(")");
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  // Motor outputs first, so the controller sees "stopped" as early as possible
  analogWriteFrequency(PWM_PIN, 100000);
  analogWrite(PWM_PIN, 0);
  pinMode(DIRECTION_PIN, OUTPUT_OPENDRAIN);
  digitalWrite(DIRECTION_PIN, HIGH);  // released = forward; idle state

  analogReadResolution(ADC_BITS);
  analogReadAveraging(4);

  Serial.begin(115200);
  delay(2000);  // Wait for Serial Monitor to connect

  Serial.printf("\n=== Teensy 4.1 Motor Node %s ===\n", MOTOR_NAME);
  Serial.printf("State topic:   %s\nCommand topic: %s\n", stateTopic, commandTopic);
  Serial.printf("Sampling at %d Hz, %d samples/packet, packet version %d\n",
                1000000 / SAMPLE_PERIOD_US, SAMPLES_PER_PACKET, PACKET_VERSION);
  Serial.println("Initializing Ethernet...");

  // Initialize Ethernet (DHCP)
  if (Ethernet.begin(mac)) {
    Serial.print("Ethernet initialized. IP: ");
    Serial.println(Ethernet.localIP());
  } else {
    Serial.println("ERROR: Ethernet.begin() failed!");
  }

  // Batched packets are bigger than PubSubClient's 256-byte default buffer
  if (!client.setBufferSize(1024)) {
    Serial.println("ERROR: could not allocate MQTT buffer!");
  }

  // Connect to MQTT broker
  connectToBroker();

  debugTimer.set(debugTimerPeriod, debugTimerCallback);

  // Start the 1 kHz control + sampling interrupt. Lower priority (higher
  // number) than the encoder pin interrupts, so encoder edges are never held
  // up by it.
  sampleTimer.priority(192);
  sampleTimer.begin(sampleISR, SAMPLE_PERIOD_US);
}

// ============================================================================
// MAIN LOOP - networking + analog sensors; control + encoder run in sampleISR()
// ============================================================================
void loop() {
  // Maintain Ethernet connection
  Ethernet.maintain();

  // If the broker link drops, cut the throttle immediately (the ISR applies
  // it within 1 ms, and keeps running while connect() blocks).
  if (!client.connected()) {
    noInterrupts();
    targetEffort = 0.0f;
    currentEffort = 0.0f;
    commandTimedOut = true;
    interrupts();
    connectToBroker();
  }

  // Process incoming MQTT messages
  client.loop();

  // Send any full batches of samples
  publishPendingSamples();

  if (timeoutEvent) {
    timeoutEvent = false;
    Serial.println("! Command timeout - target set to 0");
  }

  sampleAnalog();
  debugTimer.update();
}
