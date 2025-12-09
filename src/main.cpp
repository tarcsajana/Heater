#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>

// Pin mapping (Wemos D1 mini labels)
const uint8_t RELAY1_PIN = D8; // LIGHT (inverted)
const uint8_t RELAY4_PIN = D5; // ACC (inverted)
const uint8_t RELAY2_PIN = D7; // READY (inverted)
const uint8_t RELAY3_PIN = D6; // HORN (inverted)

// ADC pin
#define ADC_PIN A0

// Defaults
const char* MQTT_HOST = "kebnekaise.lmq.cloudamqp.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "lpomtbnx:lpomtbnx";
const char* MQTT_PASS = "9htYKaGbPej2YOYyHYGZEbkWdpDdT0Mv";
const char* DEFAULT_BASE_TOPIC = "heater";
// Default WiFi credentials (device will attempt to connect to these before starting portal)
const char* DEFAULT_WIFI_SSID = "HunorCar";
const char* DEFAULT_WIFI_PASS = "Jucymucy8581";

// EEPROM layout
struct Config {
  float voltage_divider; // multiply measured ADC voltage to get battery voltage
  float low_voltage_threshold; // in volts
  uint8_t auto_low_enabled; // 1 = enable auto precondition when below threshold
  uint32_t battery_interval_s; // seconds between battery publishes
  float battery_delta; // minimal change (V) to trigger publish
  uint32_t magic; // marker to validate
};

Config cfg;
const uint32_t CONFIG_MAGIC = 0xDEADBEEF;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
ESP8266WebServer webServer(80);
WiFiManager wifiManager;

// Persistent pointers to WiFiManagerParameter objects so they stay valid after setup()
WiFiManagerParameter *p_custom_mqtt_host = nullptr;
WiFiManagerParameter *p_custom_mqtt_port = nullptr;
WiFiManagerParameter *p_custom_mqtt_user = nullptr;
WiFiManagerParameter *p_custom_vdiv = nullptr;
WiFiManagerParameter *p_custom_lowv = nullptr;
WiFiManagerParameter *p_custom_autolow = nullptr;
WiFiManagerParameter *p_custom_batint = nullptr;
WiFiManagerParameter *p_custom_batdelt = nullptr;

// runtime state
bool relay1_state = false; // LIGHT
bool relay2_state = false; // READY
bool relay3_state = false; // HORN
bool relay4_state = false; // ACC
bool precondition_state = false;
bool portal_requested = false; // flag to request settings portal

// Precondition timers and state machine
unsigned long precond_on_until = 0; // millis until which precondition auto-on stays
unsigned long seq_step_until = 0;
int seq_step = 0;

// MQTT topics (built at setup)
String topic_base;
String topic_relay1_cmd, topic_relay1_state;
String topic_relay2_cmd, topic_relay2_state;
String topic_relay3_cmd, topic_relay3_state;
String topic_relay4_cmd, topic_relay4_state;
String topic_precondition_cmd, topic_precondition_state;
String topic_battery_state;
String topic_auto_low_cmd, topic_auto_low_state;

// helper to write inverted relay pins (LOW = ON)
void setRelay(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH);
}

void publishState(const String &topic, bool state) {
  if (mqttClient.connected()) {
    mqttClient.publish(topic.c_str(), state ? "ON" : "OFF", true);
  }
}

float readBatteryVoltage() {
  int raw = analogRead(ADC_PIN);
  // ADC raw 0..1023 corresponds to ADC reference ~1.0V on many Wemos boards
  float adc_voltage = (raw / 1023.0) * 1.0; // measured input to ADC
  return adc_voltage * cfg.voltage_divider;
}

void saveConfig() {
  cfg.magic = CONFIG_MAGIC;
  EEPROM.begin(512);
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void loadConfig() {
  EEPROM.begin(512);
  EEPROM.get(0, cfg);
  if (cfg.magic != CONFIG_MAGIC) {
    // set defaults
    memset(&cfg, 0, sizeof(cfg));
    cfg.voltage_divider = 12.0; // default multiplier; adjust in portal
    cfg.low_voltage_threshold = 10.5;
    cfg.auto_low_enabled = 0; // default: disable auto precondition on low battery
    cfg.battery_interval_s = 60; // default publish every 60s
    cfg.battery_delta = 0.10; // default publish only if change >= 0.1V
    cfg.magic = CONFIG_MAGIC;
    saveConfig();
  }
}

// Build topics and setup mqtt subscriptions
void setupTopics() {
  String node = WiFi.macAddress();
  node.replace(":", "");
  topic_base = String(DEFAULT_BASE_TOPIC) + "/" + node;

  topic_relay1_cmd = topic_base + "/relay1/set";
  topic_relay1_state = topic_base + "/relay1/state";
  topic_relay2_cmd = topic_base + "/relay2/set";
  topic_relay2_state = topic_base + "/relay2/state";
  topic_relay3_cmd = topic_base + "/relay3/set";
  topic_relay3_state = topic_base + "/relay3/state";
  topic_relay4_cmd = topic_base + "/relay4/set";
  topic_relay4_state = topic_base + "/relay4/state";

  topic_precondition_cmd = topic_base + "/precondition/set";
  topic_precondition_state = topic_base + "/precondition/state";
  topic_battery_state = topic_base + "/battery";
  topic_auto_low_cmd = topic_base + "/auto_low/set";
  topic_auto_low_state = topic_base + "/auto_low/state";

  mqttClient.subscribe(topic_relay1_cmd.c_str());
  mqttClient.subscribe(topic_relay2_cmd.c_str());
  mqttClient.subscribe(topic_relay3_cmd.c_str());
  mqttClient.subscribe(topic_relay4_cmd.c_str());
  mqttClient.subscribe(topic_precondition_cmd.c_str());
  mqttClient.subscribe(topic_auto_low_cmd.c_str());
  mqttClient.subscribe((topic_base + "/settings/set").c_str());
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  auto handleToggle = [&](String const &t_on, bool &state, uint8_t pin, const String &stateTopic){
    if (t == t_on) {
      bool newState = (msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true"));
      state = newState;
      setRelay(pin, state);
      publishState(stateTopic, state);
    }
  };

  handleToggle(topic_relay1_cmd, relay1_state, RELAY1_PIN, topic_relay1_state);
  handleToggle(topic_relay2_cmd, relay2_state, RELAY2_PIN, topic_relay2_state);
  handleToggle(topic_relay3_cmd, relay3_state, RELAY3_PIN, topic_relay3_state);
  handleToggle(topic_relay4_cmd, relay4_state, RELAY4_PIN, topic_relay4_state);

  if (t == topic_precondition_cmd) {
    bool newState = (msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true"));
    if (newState && !precondition_state) {
      // start the sequence
      precondition_state = true;
      publishState(topic_precondition_state, true);
      // ACC ON immediately
      relay4_state = true; setRelay(RELAY4_PIN, true); publishState(topic_relay4_state, true);
      // schedule READY after 3s
      seq_step = 1;
      seq_step_until = millis() + 3000UL;
    } else if (!newState && precondition_state) {
      // stop precondition: turn ACC off
      precondition_state = false;
      relay4_state = false; setRelay(RELAY4_PIN, false); publishState(topic_relay4_state, false);
      publishState(topic_precondition_state, false);
      // cancel sequence
      seq_step = 0; seq_step_until = 0;
      precond_on_until = 0;
    }
  }
  if (t == topic_auto_low_cmd) {
    bool newState = (msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true"));
    cfg.auto_low_enabled = newState ? 1 : 0;
    saveConfig();
    if (mqttClient.connected()) mqttClient.publish(topic_auto_low_state.c_str(), cfg.auto_low_enabled ? "ON" : "OFF", true);
  }
  // Settings portal trigger via MQTT
  if (t == topic_base + "/settings/set") {
    bool enable = (msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true"));
    if (enable) {
      portal_requested = true;
      Serial.println("Settings portal requested via MQTT");
    }
  }
}

// forward declaration
void publishHADiscovery();

void mqttReconnect() {
  static unsigned long last_mqtt_attempt = 0;
  static bool ha_discovery_published = false;
  // helper to convert state codes
  auto mqttStateToString = [](int8_t s)->const char* {
    switch(s) {
      case -4: return "MQTT_CONNECTION_TIMEOUT";
      case -3: return "MQTT_CONNECTION_LOST";
      case -2: return "MQTT_CONNECT_FAILED";
      case -1: return "MQTT_DISCONNECTED";
      case 0: return "MQTT_CONNECTED";
      case 1: return "MQTT_CONNECT_BAD_PROTOCOL";
      case 2: return "MQTT_CONNECT_BAD_CLIENT_ID";
      case 3: return "MQTT_CONNECT_UNAVAILABLE";
      case 4: return "MQTT_CONNECT_BAD_CREDENTIALS";
      case 5: return "MQTT_CONNECT_UNAUTHORIZED";
      default: return "MQTT_UNKNOWN";
    }
  };
  if (mqttClient.connected()) return;
  if (millis() - last_mqtt_attempt < 5000UL) return; // backoff: try at most once every 5s
  
  // Check WiFi is stable
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping MQTT attempt");
    return;
  }
  
  last_mqtt_attempt = millis();

  // build a unique client id from MAC
  String nodeId = WiFi.macAddress(); nodeId.replace(":", "");
  String clientId = String("heater-") + nodeId;
  Serial.printf("Attempting MQTT connect to %s:%u as %s\n", MQTT_HOST, MQTT_PORT, clientId.c_str());
  Serial.printf("  MQTT User: %s (len=%d)\n", MQTT_USER, strlen(MQTT_USER));
  Serial.printf("  MQTT Pass: %.4s... (len=%d)\n", MQTT_PASS, strlen(MQTT_PASS));
  
  // Set connection timeout to avoid hanging
  mqttClient.setSocketTimeout(5);  // 5 second timeout
  
  bool ok;
  ok = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS);
  Serial.printf("Connection attempt returned: %d\n", ok);
  
  if (!ok) {
    int8_t st = mqttClient.state();
    Serial.printf("MQTT connect failed: %d (%s)\n", st, mqttStateToString(st));
    return;
  }
  
  Serial.println("MQTT connected! Setting up topics...");
  if (mqttClient.connected()) {
    setupTopics();
    // publish retained initial states
    publishState(topic_relay1_state, relay1_state);
    publishState(topic_relay2_state, relay2_state);
    publishState(topic_relay3_state, relay3_state);
    publishState(topic_relay4_state, relay4_state);
    publishState(topic_precondition_state, precondition_state);
    mqttClient.publish(topic_auto_low_state.c_str(), cfg.auto_low_enabled ? "ON" : "OFF", true);
    // publish online status retained
    String status_topic = topic_base + "/status";
    mqttClient.publish(status_topic.c_str(), "online", true);
    // Publish HA discovery only once per boot (avoid repeated discovery on reconnects)
    if (!ha_discovery_published) {
      publishHADiscovery();
      ha_discovery_published = true;
    }
  }
}

// Create Home Assistant discovery payloads
void publishHADiscovery() {
  if (!mqttClient.connected()) return;
  String node = WiFi.macAddress(); node.replace(":", "");
  // Relay mappings per user request:
  // relay4 = ACC, relay2 = READY, relay3 = HORN, relay1 = LIGHT

  // helper
  auto publishSwitch = [&](const String &obj_id, const String &name, const String &cmd_topic, const String &state_topic){
    String disc = String("homeassistant/switch/") + node + "/" + obj_id + "/config";
    StaticJsonDocument<512> doc;
    doc["name"] = name;
    doc["uniq_id"] = node + "_" + obj_id;
    doc["cmd_t"] = cmd_topic;
    doc["stat_t"] = state_topic;
    doc["qos"] = 0;
    doc["pl_on"] = "ON";
    doc["pl_off"] = "OFF";
    char buf[512];
    serializeJson(doc, buf);
    mqttClient.publish(disc.c_str(), buf, false);
  };

  publishSwitch("acc", "ACC", topic_relay4_cmd, topic_relay4_state);
  publishSwitch("ready", "READY", topic_relay2_cmd, topic_relay2_state);
  publishSwitch("horn", "HORN", topic_relay3_cmd, topic_relay3_state);
  publishSwitch("light", "LIGHT", topic_relay1_cmd, topic_relay1_state);
  publishSwitch("precondition", "PRECONDITION", topic_precondition_cmd, topic_precondition_state);
  publishSwitch("auto_low", "Auto PRECOND Low", topic_auto_low_cmd, topic_auto_low_state);

  // battery sensor
  String disc_batt = String("homeassistant/sensor/") + node + "/battery/config";
  StaticJsonDocument<256> doc2;
  doc2["name"] = "Heater Battery";
  doc2["uniq_id"] = node + String("_battery");
  doc2["stat_t"] = topic_battery_state;
  doc2["unit_of_meas"] = "V";
  doc2["device_class"] = "voltage";
  char buf2[256]; serializeJson(doc2, buf2);
  mqttClient.publish(disc_batt.c_str(), buf2, false);
}

// Web server handler for settings portal trigger
void handlePortalRequest() {
  portal_requested = true;
  webServer.send(200, "text/plain", "Settings portal will start in a few seconds. Connect to 'Heater-Setup-AP' WiFi network.");
}

// Web server handler for root
void handleRoot() {
  String html = "<html><head><title>Heater Settings</title></head><body>";
  html += "<h1>Heater Control</h1>";
  html += "<p><a href='/settings'>Open Settings Portal</a></p>";
  html += "<p>Device IP: " + WiFi.localIP().toString() + "</p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

// Start settings portal
void startSettingsPortal() {
  Serial.println("Starting settings portal...");
  // Stop our own web server to avoid port conflicts with WiFiManager's captive portal
  webServer.stop();
  // Disable portal timeout (0 = no timeout) so it stays available until user finishes
  wifiManager.setConfigPortalTimeout(0);
  // Stop MQTT client to free resources while in portal
  // Note: mqttClient.disconnect() will be called if connected; reconnect after portal
  if (mqttClient.connected()) mqttClient.disconnect();

  bool res = wifiManager.startConfigPortal("Heater-Setup-AP", DEFAULT_WIFI_PASS);
  Serial.printf("Config portal finished, result=%d\n", res ? 1 : 0);

  // After portal exits, copy any changed parameters back into cfg (parameters are persistent)
  if (p_custom_vdiv) cfg.voltage_divider = atof(p_custom_vdiv->getValue());
  if (p_custom_lowv) cfg.low_voltage_threshold = atof(p_custom_lowv->getValue());
  if (p_custom_autolow) cfg.auto_low_enabled = (uint8_t)atoi(p_custom_autolow->getValue());
  if (p_custom_batint) cfg.battery_interval_s = (uint32_t)atoi(p_custom_batint->getValue());
  if (p_custom_batdelt) cfg.battery_delta = atof(p_custom_batdelt->getValue());
  saveConfig();

  // Rebuild topics and (re)subscribe
  setupTopics();

  // Restart our web server so the device UI is accessible on new network mode
  webServer.begin();
  Serial.println("Web server restarted on port 80");
  // Try to reconnect to MQTT with new settings
  mqttReconnect();

  portal_requested = false;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  // default off (remember inverted)
  setRelay(RELAY1_PIN, false);
  setRelay(RELAY2_PIN, false);
  setRelay(RELAY3_PIN, false);
  setRelay(RELAY4_PIN, false);

  loadConfig();

  wifiManager.setSaveConfigCallback([](){
    // This will be called when WiFiManager saves new WiFi credentials
  });

  // Add custom parameters for calibration (MQTT is now hardcoded)
  // Allocate WiFiManagerParameter objects on the heap so they remain valid after setup()
  char vdbuf[16]; dtostrf(cfg.voltage_divider, 4, 2, vdbuf);
  char lowvbuf[8]; dtostrf(cfg.low_voltage_threshold, 4, 2, lowvbuf);
  char autolowbuf[4]; sprintf(autolowbuf, "%u", cfg.auto_low_enabled);
  char batintbuf[8]; sprintf(batintbuf, "%u", (unsigned)cfg.battery_interval_s);
  char batdeltbuf[8]; dtostrf(cfg.battery_delta, 4, 2, batdeltbuf);

  p_custom_vdiv = new WiFiManagerParameter("vdiv", "Voltage Multiplier", vdbuf, 16);
  p_custom_lowv = new WiFiManagerParameter("lowv", "Low Voltage Threshold", lowvbuf, 8);
  p_custom_autolow = new WiFiManagerParameter("autolow", "Auto PRECOND on Low (1/0)", autolowbuf, 4);
  p_custom_batint = new WiFiManagerParameter("batint", "Battery interval (s)", batintbuf, 8);
  p_custom_batdelt = new WiFiManagerParameter("batd", "Battery delta (V)", batdeltbuf, 8);

  wifiManager.addParameter(p_custom_vdiv);
  wifiManager.addParameter(p_custom_lowv);
  wifiManager.addParameter(p_custom_autolow);
  wifiManager.addParameter(p_custom_batint);
  wifiManager.addParameter(p_custom_batdelt);

  // Start portal if no WiFi credentials
  // Try to connect using built-in default WiFi credentials first
  if (strlen(DEFAULT_WIFI_SSID) > 0) {
    Serial.printf("Trying default WiFi '%s'...\n", DEFAULT_WIFI_SSID);
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000UL) {
      delay(500);
    }
  }

  // If not connected, start configuration portal
  if (WiFi.status() != WL_CONNECTED) {
    wifiManager.autoConnect("Heater-Setup-AP", DEFAULT_WIFI_PASS);
  }

  // On success, copy back settings from persistent parameters (calibration only)
  if (p_custom_vdiv) cfg.voltage_divider = atof(p_custom_vdiv->getValue());
  if (p_custom_lowv) cfg.low_voltage_threshold = atof(p_custom_lowv->getValue());
  if (p_custom_autolow) cfg.auto_low_enabled = (uint8_t)atoi(p_custom_autolow->getValue());
  if (p_custom_batint) cfg.battery_interval_s = (uint32_t)atoi(p_custom_batint->getValue());
  if (p_custom_batdelt) cfg.battery_delta = atof(p_custom_batdelt->getValue());
  saveConfig();

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);

  // Wait a moment for WiFi to stabilize
  delay(2000);
  Serial.printf("WiFi Status: %d (0=connected)\n", WiFi.status());
  Serial.printf("WiFi IP: %s\n", WiFi.localIP().toString().c_str());

  // Setup web server for settings portal access
  webServer.on("/", handleRoot);
  webServer.on("/settings", handlePortalRequest);
  webServer.begin();
  Serial.println("Web server started on port 80");
  Serial.printf("Access via http://%s or http://192.168.4.1\n", WiFi.localIP().toString().c_str());

  // initial topics will be prepared after WiFi connected
  setupTopics();
}

void loop() {
  // Handle web server requests
  webServer.handleClient();
  
  // Check if settings portal was requested
  if (portal_requested) {
    portal_requested = false;
    delay(1000); // Give time for HTTP response to send
    startSettingsPortal();
  }
  
  if (!mqttClient.connected()) {
    mqttReconnect();
  }
  mqttClient.loop();

  // handle precondition sequence state machine
  if (seq_step == 1 && millis() >= seq_step_until) {
    // turn READY on
    relay2_state = true; setRelay(RELAY2_PIN, true); publishState(topic_relay2_state, true);
    // schedule next step: READY off after 2s
    seq_step = 2; seq_step_until = millis() + 2000UL;
  } else if (seq_step == 2 && millis() >= seq_step_until) {
    // turn READY off
    relay2_state = false; setRelay(RELAY2_PIN, false); publishState(topic_relay2_state, false);
    seq_step = 0; seq_step_until = 0;
  }

  // Precondition auto-off timer
  if (precond_on_until != 0 && millis() > precond_on_until) {
    // time expired
    precondition_state = false;
    precond_on_until = 0;
    // turn ACC off
    relay4_state = false; setRelay(RELAY4_PIN, false); publishState(topic_relay4_state, false);
    publishState(topic_precondition_state, false);
  }

  // Battery measurement at configurable interval; publish only on significant change
  static unsigned long lastBat = 0;
  static float lastPublishedBattery = 0.0;
  static bool firstBatPub = true;
  unsigned long batIntervalMs = (cfg.battery_interval_s > 0) ? (cfg.battery_interval_s * 1000UL) : 60000UL;
  if (millis() - lastBat > batIntervalMs) {
    lastBat = millis();
    float batt = readBatteryVoltage();
    float diff = batt - lastPublishedBattery;
    if (diff < 0) diff = -diff;
    bool shouldPublish = firstBatPub || (diff >= cfg.battery_delta);
    if (shouldPublish && mqttClient.connected()) {
      char buf[16]; dtostrf(batt, 4, 2, buf);
      mqttClient.publish(topic_battery_state.c_str(), buf, true);
      lastPublishedBattery = batt;
      firstBatPub = false;
    }

    // if below threshold and precondition not active and auto-low is enabled, enable for 20 minutes
    if (cfg.auto_low_enabled && batt < cfg.low_voltage_threshold && !precondition_state) {
      precondition_state = true;
      publishState(topic_precondition_state, true);
      // ACC on immediately
      relay4_state = true; setRelay(RELAY4_PIN, true); publishState(topic_relay4_state, true);
      // schedule READY after 3s
      seq_step = 1; seq_step_until = millis() + 3000UL;
      // schedule auto-off after 20 minutes
      precond_on_until = millis() + 20UL * 60UL * 1000UL;
    }
  }
}