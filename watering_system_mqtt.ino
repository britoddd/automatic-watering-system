/*
  Automatic Plant Watering System - ESP32 + MQTT over TLS
  Replaces the Blynk version. Watering logic is the same.

  Hardware wiring (unchanged):
    Soil moisture sensor VCC  -> ESP32 VIN/5V (verify sensor's voltage requirements)
    Soil moisture sensor GND  -> ESP32 GND
    Soil moisture sensor AOUT -> GPIO 34
    Relay module IN           -> GPIO 26
    Relay module VCC/GND      -> suitable supply/GND for the relay module
    Pump                      -> external suitable power supply through relay contacts

  Library: PubSubClient by Nick O'Leary (Arduino Library Manager).
  Credentials and root CA: secrets.h (same sketch folder).

  MQTT topics
    ESP32 publishes:
      garden/state   JSON, retained, every 5 s and on each change:
                     {"moisture":0-100,"raw":ADC,"pump":0/1,"auto":0/1,"fault":0/1,
                      "bursts":n,"threshold":5-60,"locked":0/1,"uptime":seconds}
      garden/status  "online" / "offline" (retained; "offline" = Last Will)

    ESP32 listens:
      garden/cmd/mode       "auto" | "manual"  -> publish with RETAIN (restored on reconnect)
      garden/cmd/threshold  "5" .. "60"        -> publish with RETAIN (restored on reconnect)
      garden/cmd/pump       "on" | "off"       -> NEVER publish with retain
                                                  (manual mode only, 10 s safety cutoff)

  IMPORTANT:
    1. Fill in secrets.h (MQTT password and root CA certificates).
    2. Calibrate DRY_VALUE and WET_VALUE for your own sensor.
    3. Test with the pump disconnected first, and confirm relay polarity.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "secrets.h"

// ---------------- Pins ----------------
const int SENSOR_PIN = 34;  // ESP32 ADC1 pin; suitable while Wi-Fi is active
const int RELAY_PIN  = 26;

// ---------------- Relay configuration ----------------
// true = relay is active LOW; false = relay is active HIGH
const bool RELAY_ACTIVE_LOW = true;

// ---------------- Calibration ----------------
// DRY_VALUE = reading in dry air/dry soil; WET_VALUE = reading in wet soil.
// This code assumes the sensor gives a HIGHER raw value when dry.
const bool CALIBRATION_MODE = false;
const int DRY_VALUE = 2440;
const int WET_VALUE = 240;
const int SENSOR_FAULT_MARGIN = 400;

// ---------------- Watering behaviour ----------------
const unsigned long PUMP_ON_MS         = 3000UL;              // automatic watering burst
const unsigned long MANUAL_MAX_RUN_MS  = 10000UL;             // manual safety cutoff
const unsigned long SOAK_TIME_MS       = 5UL * 60UL * 1000UL; // allow water to soak in
const unsigned long SENSOR_INTERVAL_MS = 5000UL;
const unsigned long WIFI_RETRY_MS      = 10000UL;
const unsigned long MQTT_RETRY_MS      = 10000UL;
const int MAX_WATERINGS_IN_A_ROW = 5;

// Can be changed with garden/cmd/threshold.
int dryThresholdPercent = 15;

// ---------------- MQTT topics ----------------
const char TOPIC_STATE[]         = "garden/state";
const char TOPIC_STATUS[]        = "garden/status";
const char TOPIC_CMD_MODE[]      = "garden/cmd/mode";
const char TOPIC_CMD_PUMP[]      = "garden/cmd/pump";
const char TOPIC_CMD_THRESHOLD[] = "garden/cmd/threshold";

// ---------------- Network clients ----------------
WiFiClientSecure net;
PubSubClient mqtt(net);

// ---------------- Runtime state ----------------
bool autoMode = true;
bool pumpRunning = false;
bool pumpWasAutomatic = false;
bool sensorValid = false;
bool lockedOut = false;
bool hasWatered = false;

int rawReading = 0;
int moisturePercent = 0;
int wateringsInARow = 0;

unsigned long pumpStartedAt = 0;
unsigned long lastWateringTime = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastSensorRead = 0;

// ---------------- Helpers ----------------
void setPumpOutput(bool on) {
  // Active-low relay: ON -> LOW, OFF -> HIGH.
  digitalWrite(RELAY_PIN, (on == RELAY_ACTIVE_LOW) ? LOW : HIGH);
}

int readSensorRaw() {
  // Average samples to reduce ADC noise without blocking for a long time.
  const int SAMPLES = 10;
  long total = 0;

  for (int i = 0; i < SAMPLES; i++) {
    total += analogRead(SENSOR_PIN);
    delay(2);
  }

  return (int)(total / SAMPLES);
}

int rawToPercent(int raw) {
  // With this calibration, dry soil is 0% and wet soil is 100%.
  long percent = (long)(DRY_VALUE - raw) * 100L /
                 (long)(DRY_VALUE - WET_VALUE);

  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  return (int)percent;
}

bool sensorLooksValid(int raw) {
  return raw >= (WET_VALUE - SENSOR_FAULT_MARGIN) &&
         raw <= (DRY_VALUE + SENSOR_FAULT_MARGIN);
}

void publishState() {
  if (!mqtt.connected()) return;

  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"moisture\":%d,\"raw\":%d,\"pump\":%d,\"auto\":%d,\"fault\":%d,"
           "\"bursts\":%d,\"threshold\":%d,\"locked\":%d,\"uptime\":%lu}",
           moisturePercent, rawReading, pumpRunning ? 1 : 0, autoMode ? 1 : 0,
           sensorValid ? 0 : 1, wateringsInARow, dryThresholdPercent,
           lockedOut ? 1 : 0, millis() / 1000UL);

  mqtt.publish(TOPIC_STATE, buf, true);  // retained: new subscribers get the last state
}

void stopPump() {
  setPumpOutput(false);

  if (pumpRunning) {
    pumpRunning = false;
    Serial.println("Pump OFF");

    if (pumpWasAutomatic) {
      lastWateringTime = millis();
      hasWatered = true;
      wateringsInARow++;
      Serial.print("Automatic watering bursts in a row: ");
      Serial.println(wateringsInARow);
    }

    pumpWasAutomatic = false;
    publishState();
  }

  pumpWasAutomatic = false;
}

bool startPump(bool automaticWatering) {
  if (pumpRunning || lockedOut || !sensorValid) return false;

  pumpRunning = true;
  pumpWasAutomatic = automaticWatering;
  pumpStartedAt = millis();
  setPumpOutput(true);

  Serial.println(automaticWatering ? "Automatic watering: pump ON"
                                   : "Manual watering: pump ON");
  publishState();
  return true;
}

void updatePumpSafety() {
  if (!pumpRunning) return;

  const unsigned long maxRunTime =
      pumpWasAutomatic ? PUMP_ON_MS : MANUAL_MAX_RUN_MS;

  if (millis() - pumpStartedAt >= maxRunTime) {
    stopPump();
  }
}

void handleSensorAndAutomaticWatering() {
  rawReading = readSensorRaw();

  Serial.print("Raw sensor reading: ");
  Serial.print(rawReading);

  if (DRY_VALUE <= WET_VALUE) {
    sensorValid = false;
    lockedOut = true;
    Serial.println(" | ERROR: DRY_VALUE must be greater than WET_VALUE.");
    stopPump();
    return;
  }

  sensorValid = sensorLooksValid(rawReading);

  if (sensorValid) {
    moisturePercent = rawToPercent(rawReading);
    Serial.print(" | Moisture: ");
    Serial.print(moisturePercent);
    Serial.println("%");
  } else {
    Serial.println(" | SENSOR FAULT: check sensor wiring/calibration.");
    // Fail safe: stop watering if the sensor reading is invalid.
    stopPump();
    return;
  }

  if (CALIBRATION_MODE) {
    Serial.println("CALIBRATION MODE: automatic watering is disabled.");
    return;
  }

  if (lockedOut) {
    Serial.println("Watering locked out. Inspect the system and reset the ESP32.");
    return;
  }

  // In manual mode, only a manual pump command can run the pump.
  if (!autoMode) return;

  // Do not restart or interrupt an active watering burst.
  if (pumpRunning) return;

  if (moisturePercent >= dryThresholdPercent) {
    wateringsInARow = 0;
    return;
  }

  // Soil is below the configured threshold.
  if (hasWatered && (millis() - lastWateringTime < SOAK_TIME_MS)) {
    Serial.println("Soil is dry; waiting for the previous watering to soak in.");
    return;
  }

  if (wateringsInARow >= MAX_WATERINGS_IN_A_ROW) {
    lockedOut = true;
    Serial.println("SAFETY LOCKOUT: soil still dry after multiple watering bursts.");
    Serial.println("Check water reservoir, tubing, sensor placement, then reset.");
    return;
  }

  startPump(true);
}

// ---------------- MQTT command handlers ----------------
void handleModeCommand(const char* msg) {
  if (strcmp(msg, "auto") == 0 || strcmp(msg, "1") == 0) {
    autoMode = true;
    // Do not leave a manual pump command active when changing modes.
    if (pumpRunning && !pumpWasAutomatic) stopPump();
    Serial.println("Mode changed to AUTOMATIC.");
  } else if (strcmp(msg, "manual") == 0 || strcmp(msg, "0") == 0) {
    autoMode = false;
    // Stop an automatic burst when the user switches to Manual mode.
    if (pumpRunning && pumpWasAutomatic) stopPump();
    Serial.println("Mode changed to MANUAL.");
  } else {
    Serial.print("Unknown mode command: ");
    Serial.println(msg);
  }
}

void handlePumpCommand(const char* msg) {
  bool on  = (strcmp(msg, "on") == 0  || strcmp(msg, "1") == 0);
  bool off = (strcmp(msg, "off") == 0 || strcmp(msg, "0") == 0);

  if (off) {
    // User explicitly turned the pump off.
    if (pumpRunning && !pumpWasAutomatic) stopPump();
    return;
  }

  if (!on) {
    Serial.print("Unknown pump command: ");
    Serial.println(msg);
    return;
  }

  if (autoMode) {
    Serial.println("Manual pump command rejected: switch to MANUAL mode first.");
    return;
  }

  if (!sensorValid || lockedOut) {
    Serial.println("Manual pump command rejected: sensor fault or safety lockout.");
    return;
  }

  startPump(false);
}

void handleThresholdCommand(const char* msg) {
  if (msg[0] < '0' || msg[0] > '9') {
    Serial.print("Invalid threshold command: ");
    Serial.println(msg);
    return;
  }

  dryThresholdPercent = constrain(atoi(msg), 5, 60);
  Serial.print("Dry threshold set to ");
  Serial.print(dryThresholdPercent);
  Serial.println("%");
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  // Copy topic and payload first: PubSubClient reuses its buffer when we publish.
  char topicCopy[48];
  strncpy(topicCopy, topic, sizeof(topicCopy) - 1);
  topicCopy[sizeof(topicCopy) - 1] = '\0';

  char msg[16];
  unsigned int n = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
  for (unsigned int i = 0; i < n; i++) {
    msg[i] = (char)tolower(payload[i]);
  }
  msg[n] = '\0';

  if (strcmp(topicCopy, TOPIC_CMD_MODE) == 0) {
    handleModeCommand(msg);
  } else if (strcmp(topicCopy, TOPIC_CMD_PUMP) == 0) {
    handlePumpCommand(msg);
  } else if (strcmp(topicCopy, TOPIC_CMD_THRESHOLD) == 0) {
    handleThresholdCommand(msg);
  }

  publishState();
}

// ---------------- Wi-Fi / MQTT connection management ----------------
void maintainConnections() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWiFiAttempt >= WIFI_RETRY_MS) {
      lastWiFiAttempt = now;
      Serial.println("Wi-Fi disconnected; retrying...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

  if (mqtt.connected()) return;

  // A TLS connect blocks for a few seconds. Never do it while the pump runs,
  // so the pump safety cutoff is never delayed.
  if (pumpRunning) return;

  if (now - lastMqttAttempt < MQTT_RETRY_MS) return;
  lastMqttAttempt = now;

  Serial.print("Connecting to MQTT broker ");
  Serial.print(MQTT_HOST);
  Serial.print("... ");

  // Last Will: the broker publishes "offline" (retained) if the ESP32 drops off.
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                   TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("connected.");
    mqtt.publish(TOPIC_STATUS, "online", true);
    mqtt.subscribe(TOPIC_CMD_MODE, 1);
    mqtt.subscribe(TOPIC_CMD_PUMP, 1);
    mqtt.subscribe(TOPIC_CMD_THRESHOLD, 1);
    publishState();
  } else {
    // -2 = network/TLS failure, 4 = bad username/password, 5 = not authorised
    Serial.print("failed, state = ");
    Serial.println(mqtt.state());
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  // Set the relay OFF before starting other subsystems.
  pinMode(RELAY_PIN, OUTPUT);
  setPumpOutput(false);

  Serial.begin(115200);
  delay(300);

  analogReadResolution(12); // ESP32 ADC: 0..4095
  pinMode(SENSOR_PIN, INPUT);

  Serial.println();
  Serial.println("Automatic Plant Watering System (MQTT) starting...");

  if (DRY_VALUE <= WET_VALUE) {
    lockedOut = true;
    Serial.println("ERROR: DRY_VALUE must be greater than WET_VALUE.");
  }

  if (CALIBRATION_MODE) {
    Serial.println("CALIBRATION MODE enabled; pump will not run automatically.");
  }

  if (strstr(ROOT_CA, "BEGIN CERTIFICATE") == nullptr) {
    Serial.println("ERROR: ROOT_CA in secrets.h is empty. TLS connection will fail.");
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();

  net.setCACert(ROOT_CA);           // verify the broker's certificate (no setInsecure)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);         // seconds; limits how long a failed connect blocks

  // Sensor/control logic runs locally, even if Wi-Fi or MQTT is unavailable.
}

void loop() {
  maintainConnections();

  if (mqtt.connected()) {
    mqtt.loop();
  }

  updatePumpSafety();

  if (millis() - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = millis();
    handleSensorAndAutomaticWatering();
    publishState();
  }
}
