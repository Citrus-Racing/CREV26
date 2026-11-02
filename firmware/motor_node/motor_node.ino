// ============================================================================
// FSAE EV26 TEST BENCH - MOTOR NODE (Teensy 4.1)
// ----------------------------------------------------------------------------
// One of these runs per motor. It:
//   - receives an effort command [-1, +1] over MQTT and drives the motor
//     controller with PWM (throttle) + an open-drain direction line
//   - reads the shaft encoder and publishes position/velocity over MQTT
//
// To flash a different motor's board, change MOTOR_ID below. Nothing else.
// ============================================================================
#include <NativeEthernet.h>
#include <PubSubClient.h>
#include <TimerEvent.h>
#include <Encoder.h>

// ============================================================================
// PER-BOARD CONFIGURATION
// ============================================================================
#define MOTOR_ID 1   // 1-4. Sets the MQTT topics, MQTT client ID and MAC address.

#define STR_(x) #x
#define STR(x) STR_(x)
const char* stateTopic   = "robot/motor/" STR(MOTOR_ID) "/state";
const char* commandTopic = "robot/motor/" STR(MOTOR_ID) "/command";
const char* mqttClientId = "Teensy_Motor_" STR(MOTOR_ID);

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, MOTOR_ID};  // unique per board
IPAddress brokerIP(192, 168, 10, 1);
const uint16_t brokerPort = 1883;

// ============================================================================
// DEFINE I/O PINS
// ============================================================================
const uint8_t PWM_PIN = 23;              // PWM "throttle" output to motor controller
const uint8_t DIRECTION_PIN = 22;        // Open-drain: released = forward, pulled to GND = reverse
const uint8_t CURRENT_SENSOR_PIN = 21;   // Analog (not wired up yet)
const uint8_t VOLTAGE_SENSOR_PIN = 20;   // Analog (not wired up yet)
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
const float RAD_PER_COUNT  = 2.0f * PI / COUNTS_PER_REV;
// Positive effort must read as positive velocity. Motor 1 needs -1 (verified
// 2026-11-02). Check this on each new board and flip it if it's backwards.
const long  ENCODER_SIGN   = -1;

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
const unsigned long COMMAND_TIMEOUT_MS = 1000;  // no command for this long -> target 0
const uint8_t PWM_MAX = 255;           // 8-bit analogWrite

// ============================================================================
// DEFINE MOTOR STATE PACKET (explicitly packed to avoid alignment padding)
// ============================================================================
struct __attribute__((packed)) MotorState {
  unsigned long timeStamp;  // micros() when the encoder was sampled - 4 bytes
  float effortCommand;      // Effort actually being output [-1.0, +1.0] (after ramp) - 4 bytes
  float current;            // Motor current (amps) - 4 bytes (uncalibrated / not wired)
  float voltage;            // Motor voltage (volts) - 4 bytes (uncalibrated / not wired)
  int32_t encoderCount;     // Raw encoder count since boot (COUNTS_PER_REV per rev) - 4 bytes
  float velocity;           // Shaft velocity, averaged over the publish period (rad/s) - 4 bytes
};
// Total: 24 bytes (no padding). Python side: struct.unpack('<Ifffif', payload)
// Position goes out as raw counts, not float radians: a float loses
// resolution once the shaft has turned a few thousand revs since boot.
// The PC converts counts to radians in double precision.

MotorState motorState = {0, 0.0f, 0.0f, 0.0f, 0, 0.0f};
float currentScale = 0.12f;
float voltageScale = 0.12f;

// ============================================================================
// DRIVE STATE
// ============================================================================
float targetEffort = 0.0f;             // Latest commanded effort [-1.0, +1.0]
float currentEffort = 0.0f;            // Effort actually being output (ramped)
unsigned long lastCommandMillis = 0;   // millis() of last valid command
bool commandTimedOut = true;           // true until the first command arrives
unsigned long lastControlMicros = 0;

// ============================================================================
// ENCODER STATE
// ============================================================================
Encoder wheelEncoder(WHEEL_ENCODER_A_PIN, WHEEL_ENCODER_B_PIN);
int32_t lastSampleCount = 0;
unsigned long lastSampleMicros = 0;

// ============================================================================
// DEFINE TIMER VARIABLES AND OBJECTS
// ============================================================================
const unsigned int controlTimerPeriod = 10;    // ms - ramp + PWM update (100 Hz)
const unsigned int publishTimerPeriod = 200;   // ms - sample encoder + publish (5 Hz)
const unsigned int analogTimerPeriod  = 100;   // ms
const unsigned int debugTimerPeriod   = 500;   // ms - Serial Monitor printout
TimerEvent controlTimer;
TimerEvent publishTimer;
TimerEvent analogTimer;
TimerEvent debugTimer;

// ============================================================================
// APPLY EFFORT - writes direction + PWM to the controller
// ============================================================================
void applyEffort(float effort) {
  // DIR only changes when effort is nonzero, so it never toggles while idle.
  // HIGH on an open-drain pin = released (forward); LOW = pulled to GND (reverse).
  if (effort > 0.0f) {
    digitalWrite(DIRECTION_PIN, HIGH);
  } else if (effort < 0.0f) {
    digitalWrite(DIRECTION_PIN, LOW);
  }

  float magnitude = fabsf(effort);
  if (magnitude > 1.0f) magnitude = 1.0f;
  analogWrite(PWM_PIN, (int)(magnitude * PWM_MAX + 0.5f));
}

// ============================================================================
// RAMP - move currentEffort toward targetEffort at the configured rates
// ============================================================================
void rampTowardTarget(float dt) {
  float diff = targetEffort - currentEffort;
  if (diff == 0.0f) return;

  // |effort| is increasing if we're starting from zero or moving away from it
  bool increasing = (currentEffort == 0.0f) || ((currentEffort > 0.0f) == (diff > 0.0f));
  float maxStep = (increasing ? RAMP_UP_PER_S : RAMP_DOWN_PER_S) * dt;
  float next = currentEffort + constrain(diff, -maxStep, maxStep);

  // On a direction change, stop at exactly zero for one tick so the new
  // direction starts from zero at the ramp-up rate.
  if ((currentEffort > 0.0f && next < 0.0f) || (currentEffort < 0.0f && next > 0.0f)) {
    next = 0.0f;
  }
  currentEffort = next;
}

// ============================================================================
// CONTROL TIMER CALLBACK (100 Hz) - watchdog, ramp, PWM output
// ============================================================================
void controlTimerCallback() {
  unsigned long now = micros();
  float dt = (now - lastControlMicros) * 1e-6f;
  lastControlMicros = now;
  if (dt > 0.1f) dt = 0.1f;  // don't jump the ramp after a stall

  // Command watchdog: if the PC stops sending, ramp to zero
  if (!commandTimedOut && (millis() - lastCommandMillis > COMMAND_TIMEOUT_MS)) {
    commandTimedOut = true;
    targetEffort = 0.0f;
    Serial.println("! Command timeout - target set to 0");
  }

  rampTowardTarget(dt);
  applyEffort(currentEffort);
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

  targetEffort = constrain(effort, -1.0f, 1.0f);
  lastCommandMillis = millis();
  if (commandTimedOut) {
    commandTimedOut = false;
    Serial.println("> Receiving commands");
  }
}

// ============================================================================
// CREATE NETWORK OBJECTS
// ============================================================================
EthernetClient ethClient;
PubSubClient client(brokerIP, brokerPort, onMessageReceived, ethClient);

// ============================================================================
// PUBLISH TIMER CALLBACK - sample encoder and publish 24-byte state packet
// ============================================================================
void publishTimerCallback() {
  // Sample encoder and timestamp together so they line up
  unsigned long now = micros();
  int32_t count = ENCODER_SIGN * wheelEncoder.read();

  float dt = (now - lastSampleMicros) * 1e-6f;
  if (dt > 0.0f) {
    // Unsigned subtraction keeps the delta right even if the count wraps
    int32_t delta = (int32_t)((uint32_t)count - (uint32_t)lastSampleCount);
    motorState.velocity = delta * RAD_PER_COUNT / dt;
  }
  lastSampleCount = count;
  lastSampleMicros = now;

  motorState.timeStamp = now;
  motorState.encoderCount = count;
  motorState.effortCommand = currentEffort;

  byte payload[sizeof(MotorState)];
  memcpy(payload, &motorState, sizeof(MotorState));
  client.publish(stateTopic, payload, sizeof(MotorState), false);
}

// ============================================================================
// ANALOG TIMER CALLBACK TO READ VOLTAGE AND CURRENT
// ============================================================================
void analogTimerCallback() {
  motorState.current = currentScale * analogRead(CURRENT_SENSOR_PIN);
  motorState.voltage = voltageScale * analogRead(VOLTAGE_SENSOR_PIN);
}

// ============================================================================
// DEBUG TIMER CALLBACK - human-readable status on the Serial Monitor
// ============================================================================
void debugTimerCallback() {
  if (!Serial) return;  // only print when the Serial Monitor is open

  long count = ENCODER_SIGN * wheelEncoder.read();
  float rpm = motorState.velocity * 60.0f / (2.0f * PI);
  Serial.printf("enc=%ld (A=%d B=%d) rev=%.2f rpm=%.0f | target=%+.2f out=%+.2f | mqtt=%s\n",
                count,
                digitalRead(WHEEL_ENCODER_A_PIN), digitalRead(WHEEL_ENCODER_B_PIN),
                count / COUNTS_PER_REV, rpm,
                targetEffort, currentEffort,
                client.connected() ? "up" : "DOWN");
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

  Serial.begin(115200);
  delay(2000);  // Wait for Serial Monitor to connect

  Serial.printf("\n=== Teensy 4.1 Motor Node %d ===\n", MOTOR_ID);
  Serial.printf("State topic:   %s\nCommand topic: %s\n", stateTopic, commandTopic);
  Serial.println("Initializing Ethernet...");

  // Initialize Ethernet (DHCP)
  if (Ethernet.begin(mac)) {
    Serial.print("Ethernet initialized. IP: ");
    Serial.println(Ethernet.localIP());
  } else {
    Serial.println("ERROR: Ethernet.begin() failed!");
  }

  // Connect to MQTT broker
  connectToBroker();

  lastSampleCount = ENCODER_SIGN * wheelEncoder.read();
  lastSampleMicros = micros();
  lastControlMicros = micros();

  controlTimer.set(controlTimerPeriod, controlTimerCallback);
  publishTimer.set(publishTimerPeriod, publishTimerCallback);
  analogTimer.set(analogTimerPeriod, analogTimerCallback);
  debugTimer.set(debugTimerPeriod, debugTimerCallback);
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // Maintain Ethernet connection
  Ethernet.maintain();

  // If the broker link drops, cut the throttle immediately. Reconnecting can
  // block for several seconds, and the ramp can't run during that time.
  if (!client.connected()) {
    targetEffort = 0.0f;
    currentEffort = 0.0f;
    applyEffort(0.0f);
    commandTimedOut = true;
    connectToBroker();
  }

  // Process incoming MQTT messages
  client.loop();

  // Run timers
  controlTimer.update();
  publishTimer.update();
  analogTimer.update();
  debugTimer.update();
}
