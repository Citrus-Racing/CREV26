#include <NativeEthernet.h>
#include <PubSubClient.h>
#include <TimerEvent.h>
#include <Encoder.h>

// ============================================================================
// NETWORK CONFIGURATION
// ============================================================================
byte mac[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
IPAddress brokerIP(192, 168, 10, 1);
const uint16_t brokerPort = 1883;

// ============================================================================
// DEFINE MQTT TOPICS
// ============================================================================
const char* stateTopic = "robot/motor/1/state";
const char* commandTopic = "robot/motor/1/command";

// ============================================================================
// DEFINE I/O PINS
// ============================================================================
const uint8_t PWM_PIN = 23;              // PWM output (0-255)
const uint8_t DIRECTION_PIN = 22;        // HIGH = forward, LOW = reverse
const uint8_t CURRENT_SENSOR_PIN = 21;   // Analog
const uint8_t VOLTAGE_SENSOR_PIN = 20;   // Analog
const uint8_t WHEEL_ENCODER_A_PIN = 18;
const uint8_t WHEEL_ENCODER_B_PIN = 17;

// ============================================================================
// DEFINE MOTOR STATE PACKET (explicitly packed to avoid alignment padding)
// ============================================================================
struct __attribute__((packed)) MotorState {
  unsigned long timeStamp;  // micros() - 4 bytes
  float effortCommand;      // Current effort being output [-1.0, +1.0] - 4 bytes
  float current;            // Motor current (amps) - 4 bytes
  float voltage;            // Motor voltage (volts) - 4 bytes
  float position;           // Motor position (radians) - 4 bytes
  float velocity;           // Motor velocity (rad/s) - 4 bytes
};
// Total: 24 bytes (no padding)

MotorState motorState = {0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
float currentScale = 0.12f;
float voltageScale = 0.12f;
float positionScale = 1.0f;
float velocityScale = 0.12f;

// ============================================================================
// PWM RAMPING VARIABLES
// ============================================================================
volatile float targetEffort = 0.0f;      // Target effort [-1.0, +1.0]
volatile float currentEffort = 0.0f;     // Current effort being output
const float rampStep = 0.05f;            // Change per ramp tick (0.05 ≈ 5% per update)
// rampStep is applied every publishTimerPeriod ms

// ============================================================================
// SET PWM METHOD - Applies currentEffort to motor (direction + PWM)
// ============================================================================
void setPWM(float effort) {
  // Convert currentEffort to PWM and direction
  uint8_t pwmValue = 0;
  uint8_t directionValue = LOW;
  
  if (effort > 0.0f) {
    // Forward: effort [0.0, 1.0] → PWM [0, 255]
    pwmValue = (uint8_t)(effort * 255.0f);
    directionValue = HIGH;
  } else if (effort < 0.0f) {
    // Reverse: effort [-1.0, 0.0] → PWM [255, 0]
    pwmValue = (uint8_t)((-effort) * 255.0f);
    directionValue = LOW;
  }
  // else: effort == 0.0 → pwmValue stays 0, motor stops
  
  // Apply to motor
  digitalWrite(DIRECTION_PIN, directionValue);
  analogWrite(PWM_PIN, pwmValue);
}

// ============================================================================
// RAMP PWM TOWARD TARGET - Called from publish timer
// ============================================================================
void rampPWMTowardTarget() {
  // Move currentEffort toward targetEffort
  if (currentEffort < targetEffort) {
    currentEffort = min(currentEffort + rampStep, targetEffort);
  } else if (currentEffort > targetEffort) {
    currentEffort = max(currentEffort - rampStep, targetEffort);
  }
  
  // Apply the ramped effort to the motor
  setPWM(currentEffort);
  
  // Update motorState with current effort for telemetry
  motorState.effortCommand = currentEffort;
}

// ============================================================================
// DEFINE TIMER VARIABLES AND OBJECTS
// ============================================================================
const unsigned int publishTimerPeriod = 200;
const unsigned int analogTimerPeriod = 100;
const unsigned int encoderTimerPeriod = 100;
TimerEvent publishTimer;
TimerEvent analogTimer;
TimerEvent encoderTimer;

// ============================================================================
// DEFINE ENCODER VARIABLES AND OBJECTS
// ============================================================================
long wheelEncoderCount  = 0;
Encoder wheelEncoder(WHEEL_ENCODER_A_PIN, WHEEL_ENCODER_B_PIN);

// ============================================================================
// MQTT MESSAGE CALLBACK (Effort Command)
// ============================================================================
void onMessageReceived(char* topic, byte* payload, unsigned int length) {
  // Verify we're receiving the correct topic
  if (strcmp(topic, commandTopic) != 0) {
    return;
  }
  
  // Extract float from 4-byte payload
  if (length != sizeof(float)) {
    return;
  }
  
  // Cast payload to float
  float effort = 0.0f;
  memcpy(&effort, payload, sizeof(float));
 }

// ============================================================================
// CREATE NETWORK OBJECTS
// ============================================================================
EthernetClient ethClient;
PubSubClient client(brokerIP, brokerPort, onMessageReceived, ethClient);

// ============================================================================
// PUBLISH TIMER CALLBACK TO PUBLISH MOTOR STATE (16-byte binary packet)
// ============================================================================
void publishTimerCallback() {
  // Ramp the PWM toward target
  rampPWMTowardTarget();
  
  // Update timestamp
  motorState.timeStamp = micros();
  
  // Cast struct to byte array and publish
  byte payload[sizeof(MotorState)];
  memcpy(payload, &motorState, sizeof(MotorState));
  
  // Publish as binary
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
// ENCODER TIMER CALLBACK TO READ AND FILTER MOTOR ENCODERS
// ============================================================================
void encoderTimerCallback() {
  wheelEncoderCount = wheelEncoder.read();
  motorState.position = positionScale * wheelEncoderCount;
  motorState.velocity = velocityScale * 1.0f;
}

// ============================================================================
// CONNECT TO MQTT BROKER
// ============================================================================
void connectToBroker() {
  static unsigned long lastAttempt = 0;
  static uint8_t attemptCount = 0;
  
  // Throttle connection attempts (don't hammer the broker)
  if (millis() - lastAttempt < 5000) {
    return;
  }
  lastAttempt = millis();
  
  Serial.print("Connecting to MQTT broker at ");
  Serial.print(brokerIP);
  Serial.print(":");
  Serial.println(brokerPort);
  
  if (client.connect("Teensy_Motor_1")) {
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
  Serial.begin(115200);
  delay(2000);  // Wait for Serial Monitor to connect
  
  Serial.println("\n=== Teensy 4.1 Motor Control System ===");
  Serial.println("Initializing Ethernet...");
  
  // Configure pins
  analogWriteFrequency(PWM_PIN, 100000);
  analogWrite(PWM_PIN, 0);
  pinMode(DIRECTION_PIN, OUTPUT_OPENDRAIN);
  digitalWrite(DIRECTION_PIN, LOW);
  
  // Initialize Ethernet
  if (Ethernet.begin(mac)) {
    Serial.print("Ethernet initialized. IP: ");
    Serial.println(Ethernet.localIP());
  } else {
    Serial.println("ERROR: Ethernet.begin() failed!");
  }
  
  // Connect to MQTT broker
  connectToBroker();

  publishTimer.set(publishTimerPeriod, publishTimerCallback);
  analogTimer.set(analogTimerPeriod, analogTimerCallback);
  encoderTimer.set(encoderTimerPeriod, encoderTimerCallback);
}

// ============================================================================
// MAIN LOOP
// ============================================================================
void loop() {
  // Maintain Ethernet connection
  Ethernet.maintain();
  
  // Reconnect to broker if disconnected
  if (!client.connected()) {
    connectToBroker();
  }
  
  // Process incoming MQTT messages
  client.loop();
  
  // Run timers
  publishTimer.update();
  analogTimer.update();
  encoderTimer.update();
}

