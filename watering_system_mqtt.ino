/*
  Automatic Plant Watering System - ESP32 + MQTT over TLS + offline hotspot
  Watering logic is the same as the Blynk version.

  Hardware wiring (unchanged):
    Soil moisture sensor VCC  -> ESP32 VIN/5V (verify sensor's voltage requirements)
    Soil moisture sensor GND  -> ESP32 GND
    Soil moisture sensor AOUT -> GPIO 34
    Relay module IN           -> GPIO 26
    Relay module VCC/GND      -> suitable supply/GND for the relay module
    Pump                      -> external suitable power supply through relay contacts

  Library: PubSubClient by Nick O'Leary (Arduino Library Manager).
  WebServer is part of the ESP32 Arduino core.
  Credentials and root CA: secrets.h (same sketch folder).

  ---------------- Offline control ----------------
  The ESP32 always runs its own hotspot (AP_SSID in secrets.h).
  Connect a phone to it and open http://192.168.4.1 (login: LOCAL_USER / LOCAL_PASS).
  This works with no router and no internet.
  When the ESP32 is also on your router, the same page is at its router IP.

  ---------------- Changing the router Wi-Fi ----------------
  On that page, "Router Wi-Fi" sets the network the ESP32 joins for internet
  (a home router or a phone hotspot). It is saved in flash (Preferences) and
  survives reboots. With nothing saved, WIFI_SSID / WIFI_PASSWORD from secrets.h
  are used. Your phone may drop off the hotspot for a moment while the ESP32
  joins the new network (the hotspot moves to the router's channel).

  ---------------- MQTT topics ----------------
    ESP32 publishes:
      garden/state   JSON, retained, every 5 s and on each change:
                     {"moisture":0-100,"raw":ADC,"pump":0/1,"auto":0/1,"fault":0/1,
                      "bursts":n,"threshold":5-60,"locked":0/1,"uptime":seconds}
      garden/status  "online" / "offline" (retained; "offline" = Last Will)

    ESP32 listens:
      garden/cmd/mode       "auto" | "manual"  -> publish with RETAIN
      garden/cmd/threshold  "5" .. "60"        -> publish with RETAIN
      garden/cmd/pump       "on" | "off"       -> NEVER publish with retain

    When mode or threshold is changed on the local page, the ESP32 also
    publishes the new value (retained) to garden/cmd/mode or
    garden/cmd/threshold, so the broker does not undo the local change.
    If MQTT is offline at that time, it publishes on the next connect.

  IMPORTANT:
    1. Fill in secrets.h (passwords and root CA certificates).
    2. Calibrate DRY_VALUE and WET_VALUE for your own sensor.
    3. Test with the pump disconnected first, and confirm relay polarity.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
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

// ---------------- Water tank level (analog level strip, optional) ----------------
// Set WATER_LEVEL_ENABLED to true once the strip is wired. While false, none of this
// runs and garden/state has no water fields (the dashboards hide the tank parts).
//
// Wiring: strip "+" -> WATER_LEVEL_POWER_PIN, "-" -> GND, "S" -> WATER_LEVEL_PIN.
// The strip is powered only for the few milliseconds of each reading: a strip with
// constant current through the water corrodes within weeks.
// Mount it so its bottom end sits at the lowest level the pump may still draw from.
const bool WATER_LEVEL_ENABLED   = false;
const int  WATER_LEVEL_PIN       = 35;  // ADC1, input-only pin (fine for analog)
const int  WATER_LEVEL_POWER_PIN = 25;  // 3.3 V from a GPIO keeps the signal inside the ADC range
// Calibration (use CALIBRATION_MODE to see raw values on the Serial Monitor):
const int  WATER_EMPTY_RAW = 0;         // raw reading with the strip out of the water
const int  WATER_FULL_RAW  = 1800;      // raw reading with the strip submerged to its top mark
// Below LOW the tank counts as empty and the pump is blocked (no dry running).
// It unblocks only above OK, so a level hovering at the limit doesn't flap.
const int  WATER_LOW_PERCENT = 15;
const int  WATER_OK_PERCENT  = 25;

// ---------------- Watering behaviour ----------------
const unsigned long PUMP_ON_MS         = 3000UL;              // automatic watering burst
const unsigned long MANUAL_MAX_RUN_MS  = 10000UL;             // manual safety cutoff
const unsigned long SOAK_TIME_MS       = 5UL * 60UL * 1000UL; // allow water to soak in
const unsigned long SENSOR_INTERVAL_MS = 5000UL;
const unsigned long WIFI_RETRY_MS      = 10000UL;
const unsigned long MQTT_RETRY_MS      = 10000UL;
const int MAX_WATERINGS_IN_A_ROW = 5;

// Can be changed with garden/cmd/threshold or the local page.
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
WebServer server(80);
Preferences prefs;

// Router Wi-Fi in use: saved in flash, or the secrets.h defaults.
char wifiSsid[33];  // 802.11 limits: SSID <= 32 bytes, WPA2 passphrase <= 63
char wifiPass[64];

// ---------------- Runtime state ----------------
bool autoMode = true;
bool pumpRunning = false;
bool pumpWasAutomatic = false;
bool sensorValid = false;
bool lockedOut = false;
bool hasWatered = false;

// Local changes that must still be sent to the broker (retained).
bool modeNeedsSync = false;
bool thresholdNeedsSync = false;

int rawReading = 0;
int moisturePercent = 0;
int wateringsInARow = 0;

// Water tank (only used when WATER_LEVEL_ENABLED)
int waterRaw = 0;
int waterPercent = 0;
bool tankLow = false;

unsigned long pumpStartedAt = 0;
unsigned long lastWateringTime = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastSensorRead = 0;

// ---------------- Local web page ----------------
const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Garden</title>
<style>
body{font-family:sans-serif;max-width:420px;margin:auto;padding:16px;background:#f4f6f4;color:#222}
.card{background:#fff;border-radius:10px;padding:14px;margin-bottom:12px;box-shadow:0 1px 3px #0002}
.big{font-size:48px;font-weight:bold}
.row{display:flex;justify-content:space-between;margin:4px 0}
button{width:100%;padding:12px;margin-top:8px;font-size:16px;border:0;border-radius:8px;background:#2e7d32;color:#fff}
button.alt{background:#555}
button:disabled{background:#bbb}
.warn{color:#b71c1c;font-weight:bold;margin-top:8px}
</style></head><body>
<h2>Garden watering</h2>
<div class="card">
<div>Soil moisture</div><div class="big" id="m">--%</div>
<div class="row"><span>Raw ADC</span><span id="raw">-</span></div>
<div class="row"><span>Pump</span><span id="pump">-</span></div>
<div class="row"><span>Mode</span><span id="mode">-</span></div>
<div class="row"><span>Bursts in a row</span><span id="b">-</span></div>
<div class="row" id="tr" style="display:none"><span>Water tank</span><span id="tank">-</span></div>
<div class="row"><span>Router Wi-Fi</span><span id="wifi">-</span></div>
<div class="row"><span>Internet (MQTT)</span><span id="mq">-</span></div>
<div id="w" class="warn"></div>
</div>
<div class="card">
<button id="ba" onclick="cmd('mode','auto')">Automatic mode</button>
<button id="bm" class="alt" onclick="cmd('mode','manual')">Manual mode</button>
<button id="bp" onclick="cmd('pump','on')">Water now (max 10 s)</button>
<button class="alt" onclick="cmd('pump','off')">Stop pump</button>
</div>
<div class="card">
<div>Dry threshold: <b id="tv">-</b>%</div>
<input type="range" id="ts" min="5" max="60" style="width:100%"
 oninput="tv.textContent=this.value" onchange="cmd('threshold',this.value)">
</div>
<div class="card">
<div>Router Wi-Fi (for internet)</div>
<input id="ss" placeholder="Network name (SSID)" maxlength="32" style="width:100%;padding:10px;margin-top:8px;box-sizing:border-box">
<input id="sp" type="password" placeholder="Password (empty = open network)" maxlength="63" style="width:100%;padding:10px;margin-top:8px;box-sizing:border-box">
<button onclick="saveWifi()">Save and connect</button>
<div id="ws" style="margin-top:8px"></div>
</div>
<script>
const $=id=>document.getElementById(id);
let msg='',msgUntil=0;
function show(s){
 $('m').textContent=s.fault?'--%':s.moisture+'%';
 $('raw').textContent=s.raw;
 $('pump').textContent=s.pump?'ON':'OFF';
 $('mode').textContent=s.auto?'Automatic':'Manual';
 $('b').textContent=s.bursts;
 $('wifi').textContent=s.wifi?'Connected ('+s.ip+')':'Not connected';
 $('mq').textContent=s.mqtt?'Connected':'Offline';
 if(document.activeElement!==$('ts')){$('ts').value=s.threshold;$('tv').textContent=s.threshold;}
 if('water' in s){$('tr').style.display='';$('tank').textContent=s.water+'%'+(s.tankLow?' (LOW)':'');}
 $('bp').disabled=s.auto||s.fault||s.locked||s.pump||s.tankLow;
 let w=[];
 if(s.tankLow)w.push('WATER TANK LOW: refill it. The pump is blocked until then.');
 if(s.locked)w.push('SAFETY LOCKOUT: soil still dry after several bursts. Check water, tubing and sensor, then reset the ESP32.');
 if(s.fault)w.push('SENSOR FAULT: check sensor wiring or calibration.');
 if(Date.now()<msgUntil)w.push(msg);
 $('w').textContent=w.join(' ');
}
async function load(){
 try{const r=await fetch('/api/state');show(await r.json());}
 catch(e){$('w').textContent='No connection to the ESP32.';}
}
async function cmd(k,v){
 try{
  const r=await fetch('/api/'+k,{method:'POST',
   headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body:'value='+encodeURIComponent(v)});
  const j=await r.json();
  if(!r.ok){msg=j.error||'Command rejected.';msgUntil=Date.now()+5000;}
 }catch(e){msg='Command failed.';msgUntil=Date.now()+5000;}
 load();
}
async function saveWifi(){
 const s=$('ss').value.trim(),p=$('sp').value;
 try{
  const r=await fetch('/api/wifi',{method:'POST',
   headers:{'Content-Type':'application/x-www-form-urlencoded'},
   body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)});
  const j=await r.json();
  $('ws').textContent=r.ok?'Saved. Connecting... watch "Router Wi-Fi" above (your phone may reconnect to the hotspot).':(j.error||'Rejected.');
  if(r.ok)$('sp').value='';
 }catch(e){$('ws').textContent='No answer. Reconnect your phone to the hotspot and reload this page.';}
}
fetch('/api/wifi').then(r=>r.text()).then(t=>{$('ss').value=t;}).catch(()=>{});
setInterval(load,2000);load();
</script></body></html>
)HTML";

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

// Power the strip, average a few samples, power it off again.
// A disconnected strip reads about 0, which counts as "low": the safe side.
void readWaterLevel() {
  digitalWrite(WATER_LEVEL_POWER_PIN, HIGH);
  delay(10);  // let the reading settle
  long total = 0;
  for (int i = 0; i < 10; i++) {
    total += analogRead(WATER_LEVEL_PIN);
    delay(2);
  }
  digitalWrite(WATER_LEVEL_POWER_PIN, LOW);

  waterRaw = (int)(total / 10);
  if (WATER_FULL_RAW <= WATER_EMPTY_RAW) {  // bad calibration: stay on the safe side
    waterPercent = 0;
    tankLow = true;
    stopPump();
    return;
  }
  long p = (long)(waterRaw - WATER_EMPTY_RAW) * 100L / (long)(WATER_FULL_RAW - WATER_EMPTY_RAW);
  waterPercent = (int)constrain(p, 0L, 100L);

  bool wasLow = tankLow;
  if (waterPercent <= WATER_LOW_PERCENT) tankLow = true;
  else if (waterPercent > WATER_OK_PERCENT) tankLow = false;

  Serial.print("Water tank raw: ");
  Serial.print(waterRaw);
  Serial.print(" | Level: ");
  Serial.print(waterPercent);
  Serial.println(tankLow ? "% (LOW)" : "%");

  if (tankLow && !wasLow) {
    Serial.println("WATER TANK LOW: pump blocked until the tank is refilled.");
    stopPump();
  }
}

// forWeb = true adds link status fields for the local page.
void buildStateJson(char* buf, size_t len, bool forWeb) {
  int n = snprintf(buf, len,
                   "{\"moisture\":%d,\"raw\":%d,\"pump\":%d,\"auto\":%d,\"fault\":%d,"
                   "\"bursts\":%d,\"threshold\":%d,\"locked\":%d,\"uptime\":%lu",
                   moisturePercent, rawReading, pumpRunning ? 1 : 0, autoMode ? 1 : 0,
                   sensorValid ? 0 : 1, wateringsInARow, dryThresholdPercent,
                   lockedOut ? 1 : 0, millis() / 1000UL);

  // Water fields exist only with a sensor; their absence tells the dashboards to hide the tank.
  if (WATER_LEVEL_ENABLED && n > 0 && (size_t)n < len) {
    n += snprintf(buf + n, len - n, ",\"water\":%d,\"waterRaw\":%d,\"tankLow\":%d",
                  waterPercent, waterRaw, tankLow ? 1 : 0);
  }

  if (forWeb && n > 0 && (size_t)n < len) {
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    n += snprintf(buf + n, len - n, ",\"wifi\":%d,\"mqtt\":%d,\"ip\":\"%s\"",
                  wifiUp ? 1 : 0, mqtt.connected() ? 1 : 0,
                  wifiUp ? WiFi.localIP().toString().c_str() : "");
  }

  if (n > 0 && (size_t)n < len) {
    snprintf(buf + n, len - n, "}");
  }
}

void publishState() {
  if (!mqtt.connected()) return;

  char buf[220];
  buildStateJson(buf, sizeof(buf), false);
  mqtt.publish(TOPIC_STATE, buf, true);  // retained: new subscribers get the last state
}

// Send locally changed settings to the broker as retained commands,
// so the retained values on the broker match the device.
void syncLocalSettings() {
  if (!mqtt.connected()) return;

  if (modeNeedsSync) {
    mqtt.publish(TOPIC_CMD_MODE, autoMode ? "auto" : "manual", true);
    modeNeedsSync = false;
  }

  if (thresholdNeedsSync) {
    char t[8];
    snprintf(t, sizeof(t), "%d", dryThresholdPercent);
    mqtt.publish(TOPIC_CMD_THRESHOLD, t, true);
    thresholdNeedsSync = false;
  }
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
  if (WATER_LEVEL_ENABLED && tankLow) return false;  // never run the pump dry

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
  if (WATER_LEVEL_ENABLED) readWaterLevel();

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
  // An empty tank is not "the soil won't get wet": skip without counting
  // toward the safety lockout, and water again once the tank is refilled.
  if (WATER_LEVEL_ENABLED && tankLow) {
    Serial.println("Soil is dry but the water tank is low; not watering.");
    return;
  }

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

// ---------------- Command handlers (shared by MQTT and the local page) ----------------
// Each returns nullptr on success, or an error message.
const char* handleModeCommand(const char* msg) {
  if (strcmp(msg, "auto") == 0 || strcmp(msg, "1") == 0) {
    autoMode = true;
    // Do not leave a manual pump command active when changing modes.
    if (pumpRunning && !pumpWasAutomatic) stopPump();
    Serial.println("Mode changed to AUTOMATIC.");
    return nullptr;
  }

  if (strcmp(msg, "manual") == 0 || strcmp(msg, "0") == 0) {
    autoMode = false;
    // Stop an automatic burst when the user switches to Manual mode.
    if (pumpRunning && pumpWasAutomatic) stopPump();
    Serial.println("Mode changed to MANUAL.");
    return nullptr;
  }

  Serial.print("Unknown mode command: ");
  Serial.println(msg);
  return "Unknown mode. Use auto or manual.";
}

const char* handlePumpCommand(const char* msg) {
  bool on  = (strcmp(msg, "on") == 0  || strcmp(msg, "1") == 0);
  bool off = (strcmp(msg, "off") == 0 || strcmp(msg, "0") == 0);

  if (off) {
    // User explicitly turned the pump off.
    if (pumpRunning && !pumpWasAutomatic) stopPump();
    return nullptr;
  }

  if (!on) {
    Serial.print("Unknown pump command: ");
    Serial.println(msg);
    return "Unknown pump command. Use on or off.";
  }

  if (autoMode) {
    Serial.println("Manual pump command rejected: switch to MANUAL mode first.");
    return "Switch to Manual mode first.";
  }

  if (!sensorValid || lockedOut) {
    Serial.println("Manual pump command rejected: sensor fault or safety lockout.");
    return "Rejected: sensor fault or safety lockout.";
  }

  if (WATER_LEVEL_ENABLED && tankLow) {
    Serial.println("Manual pump command rejected: water tank low.");
    return "Rejected: water tank low. Refill it first.";
  }

  if (!startPump(false)) {
    return "The pump is already running.";
  }
  return nullptr;
}

const char* handleThresholdCommand(const char* msg) {
  if (msg[0] < '0' || msg[0] > '9') {
    Serial.print("Invalid threshold command: ");
    Serial.println(msg);
    return "Threshold must be a number from 5 to 60.";
  }

  dryThresholdPercent = constrain(atoi(msg), 5, 60);
  Serial.print("Dry threshold set to ");
  Serial.print(dryThresholdPercent);
  Serial.println("%");
  return nullptr;
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

// ---------------- Local web server handlers ----------------
bool checkAuth() {
  if (!server.authenticate(LOCAL_USER, LOCAL_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void sendStateResponse(int code) {
  char buf[300];
  buildStateJson(buf, sizeof(buf), true);
  server.send(code, "application/json", buf);
}

void sendError(int code, const char* error) {
  char buf[160];
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", error);
  server.send(code, "application/json", buf);
}

// Read the "value" form field, lowercased, into out.
void readValueArg(char* out, size_t len) {
  String v = server.arg("value");
  v.trim();
  v.toLowerCase();
  strncpy(out, v.c_str(), len - 1);
  out[len - 1] = '\0';
}

void handleRoot() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleApiState() {
  if (!checkAuth()) return;
  sendStateResponse(200);
}

void handleApiMode() {
  if (!checkAuth()) return;
  char v[16];
  readValueArg(v, sizeof(v));

  const char* err = handleModeCommand(v);
  if (err) { sendError(400, err); return; }

  modeNeedsSync = true;
  syncLocalSettings();
  publishState();
  sendStateResponse(200);
}

void handleApiPump() {
  if (!checkAuth()) return;
  char v[16];
  readValueArg(v, sizeof(v));

  const char* err = handlePumpCommand(v);
  if (err) { sendError(409, err); return; }

  publishState();
  sendStateResponse(200);
}

void handleApiThreshold() {
  if (!checkAuth()) return;
  char v[16];
  readValueArg(v, sizeof(v));

  const char* err = handleThresholdCommand(v);
  if (err) { sendError(400, err); return; }

  thresholdNeedsSync = true;
  syncLocalSettings();
  publishState();
  sendStateResponse(200);
}

// ---------------- Router Wi-Fi settings (saved in flash) ----------------
void loadWifiConfig() {
  // Read-only open fails if nothing was ever saved -> use the secrets.h defaults.
  bool saved = prefs.begin("wifi", true) && prefs.isKey("ssid");
  if (saved) {
    prefs.getString("ssid", wifiSsid, sizeof(wifiSsid));
    prefs.getString("pass", wifiPass, sizeof(wifiPass));
  } else {
    strncpy(wifiSsid, WIFI_SSID, sizeof(wifiSsid) - 1);
    strncpy(wifiPass, WIFI_PASSWORD, sizeof(wifiPass) - 1);
  }
  prefs.end();
  Serial.print(saved ? "Router Wi-Fi (saved): " : "Router Wi-Fi (secrets.h): ");
  Serial.println(wifiSsid);
}

void beginRouterWifi() {
  // An empty password means an open network.
  WiFi.begin(wifiSsid, wifiPass[0] ? wifiPass : nullptr);
}

void handleApiWifiGet() {
  if (!checkAuth()) return;
  server.send(200, "text/plain", wifiSsid);  // plain text: SSIDs may contain quotes
}

void handleApiWifiSet() {
  if (!checkAuth()) return;
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();  // the password is NOT trimmed: spaces can be part of it

  if (ssid.length() < 1 || ssid.length() > 32) {
    sendError(400, "Network name must be 1 to 32 characters.");
    return;
  }
  if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63)) {
    sendError(400, "Password must be 8 to 63 characters (or empty for an open network).");
    return;
  }

  strncpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid) - 1);
  wifiSsid[sizeof(wifiSsid) - 1] = '\0';
  strncpy(wifiPass, pass.c_str(), sizeof(wifiPass) - 1);
  wifiPass[sizeof(wifiPass) - 1] = '\0';

  prefs.begin("wifi", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.end();
  Serial.print("Router Wi-Fi changed to: ");
  Serial.println(wifiSsid);

  // Answer first: joining the new network can move the hotspot's channel
  // and briefly drop the phone.
  server.send(200, "application/json", "{\"ok\":true}");
  delay(100);

  // A clean disconnect skips the Last Will, so report "offline" ourselves.
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_STATUS, "offline", true);
    mqtt.disconnect();
  }
  WiFi.disconnect();
  beginRouterWifi();
  lastWiFiAttempt = millis();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/wifi", HTTP_GET, handleApiWifiGet);
  server.on("/api/wifi", HTTP_POST, handleApiWifiSet);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/mode", HTTP_POST, handleApiMode);
  server.on("/api/pump", HTTP_POST, handleApiPump);
  server.on("/api/threshold", HTTP_POST, handleApiThreshold);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ---------------- Wi-Fi / MQTT connection management ----------------
void maintainConnections() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    // A router reconnect scans all channels and disturbs the hotspot.
    // Do not retry while a phone is connected to the hotspot.
    if (WiFi.softAPgetStationNum() > 0) return;

    if (now - lastWiFiAttempt >= WIFI_RETRY_MS) {
      lastWiFiAttempt = now;
      Serial.println("Router Wi-Fi disconnected; retrying...");
      WiFi.disconnect();
      beginRouterWifi();
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
    // Send local changes made while offline BEFORE subscribing,
    // so old retained commands do not undo them.
    syncLocalSettings();
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

  if (WATER_LEVEL_ENABLED) {
    pinMode(WATER_LEVEL_POWER_PIN, OUTPUT);
    digitalWrite(WATER_LEVEL_POWER_PIN, LOW);  // strip unpowered between readings
    pinMode(WATER_LEVEL_PIN, INPUT);
    if (WATER_FULL_RAW <= WATER_EMPTY_RAW) {
      Serial.println("ERROR: WATER_FULL_RAW must be greater than WATER_EMPTY_RAW.");
    }
  }

  Serial.println();
  Serial.println("Automatic Plant Watering System (MQTT + hotspot) starting...");

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

  // Hotspot + router connection at the same time.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);  // reconnects are handled in maintainConnections()

  if (strlen(AP_PASSWORD) < 8) {
    Serial.println("ERROR: AP_PASSWORD must have at least 8 characters. Hotspot not started.");
  } else if (WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.print("Hotspot started: ");
    Serial.print(AP_SSID);
    Serial.print("  ->  http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: hotspot failed to start.");
  }

  loadWifiConfig();
  beginRouterWifi();
  lastWiFiAttempt = millis();

  setupWebServer();

  net.setCACert(ROOT_CA);           // verify the broker's certificate (no setInsecure)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);         // seconds; limits how long a failed connect blocks

  // Sensor/control logic runs locally, even with no router and no internet.
}

void loop() {
  maintainConnections();

  if (mqtt.connected()) {
    mqtt.loop();
  }

  server.handleClient();
  updatePumpSafety();

  if (millis() - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = millis();
    handleSensorAndAutomaticWatering();
    publishState();
  }
}
