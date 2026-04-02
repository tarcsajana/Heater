#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <Updater.h>
#include <time.h>
#include <cmath>

// Firmware version
const char* FIRMWARE_VERSION = "1.1";
const char* FIRMWARE_BUILD_TIME = __DATE__ " " __TIME__;
const char* FIRMWARE_URL = "https://github.com/tarcsajana/Heater/releases/latest/download/firmware.bin";
const char* GITHUB_TOKEN = "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";  // Empty for public repo, set to PAT for private repo

// Pin mapping (Wemos D1 mini labels)
const uint8_t RELAY1_PIN = D8; // LIGHT (inverted)
const uint8_t RELAY4_PIN = D5; // ACC (inverted) - Changed from D5
const uint8_t RELAY2_PIN = D7; // READY (inverted)
const uint8_t RELAY3_PIN = D6; // ILLUMINATION / radio brightness wire (inverted)

// ADC pin
#define ADC_PIN A0

// Sun automation for OSLO, Norway
const double OSLO_LATITUDE = 59.9139;  // N
const double OSLO_LONGITUDE = 10.7522; // E
bool sun_automation_enabled = true;
bool is_dark_now = false;
unsigned long last_sun_check = 0;
const unsigned long SUN_CHECK_INTERVAL = 60000; // check every minute

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
bool relay3_state = false; // ILLUMINATION (radio brightness wire)
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

// OTA topics
String topic_firmware_cmd;
String topic_firmware_state;
String topic_power_cmd;
String topic_down_cmd;
String topic_up_cmd;
String topic_mode_cmd;
String topic_status;
const char* LEGACY_FIRMWARE_CMD = "heater/firmware/set";

// helper to write inverted relay pins (LOW = ON)
void setRelay(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH);
}

// Publish firmware info to MQTT
void publishFirmwareInfo() {
  if (!mqttClient.connected()) return;
  StaticJsonDocument<256> doc;
  doc["fw_version"] = FIRMWARE_VERSION;
  doc["build_time"] = FIRMWARE_BUILD_TIME;
  char buf[256];
  serializeJson(doc, buf);
  mqttClient.publish(topic_firmware_state.c_str(), buf, true);
}

// HTTP firmware update from URL
void performSelfUpdate(String url) {
  Serial.printf("Starting firmware update from: %s\n", url.c_str());

  HTTPClient http;
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  bool isHttps = url.startsWith("https://");
  std::unique_ptr<BearSSL::WiFiClientSecure> secureClient;
  std::unique_ptr<WiFiClient> plainClient;

  if (isHttps) {
    secureClient.reset(new BearSSL::WiFiClientSecure());
    secureClient->setInsecure(); // allow GitHub TLS without pinned cert
    Serial.println("[OTA DEBUG] Using HTTPS client");
    if (!http.begin(*secureClient, url)) {
      Serial.println("HTTPS begin failed");
      return;
    }
  } else {
    plainClient.reset(new WiFiClient());
    Serial.println("[OTA DEBUG] Using HTTP client");
    if (!http.begin(*plainClient, url)) {
      Serial.println("HTTP begin failed");
      return;
    }
  }

  Serial.println("[OTA DEBUG] HTTP client initialized, sending GET request");
  int httpCode = http.GET();
  Serial.printf("[OTA DEBUG] HTTP response code: %d\n", httpCode);
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP error: %d\n", httpCode);
    http.end();
    return;
  }

  int len = http.getSize();
  Serial.printf("[OTA DEBUG] Content-Length: %d bytes\n", len);
  if (len > 0) {
    Serial.printf("Firmware size: %d bytes\n", len);
  } else {
    Serial.println("Content-Length unknown, using max sketch space");
  }

  size_t updateSize = (len > 0) ? (size_t)len : ((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
  Serial.printf("[OTA DEBUG] Update size to allocate: %u bytes\n", updateSize);
  
  if (!Update.begin(updateSize)) {
    Serial.println("Update.begin() failed");
    http.end();
    return;
  }

  Serial.println("[OTA DEBUG] Update.begin() succeeded, starting write stream");
  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  Serial.printf("[OTA DEBUG] Written %u bytes to Update\n", written);
  if (len > 0 && written != (size_t)len) {
    Serial.printf("Written %zu bytes, expected %d\n", written, len);
    Update.end();
    return;
  }

  if (!Update.end()) {
    Serial.println("Update.end() failed");
    return;
  }

  if (Update.isFinished()) {
    Serial.println("Self-update OK - rebooting");
    delay(1000);
    ESP.restart();
  } else {
    Serial.println("Update not finished");
  }
}

// Check if sun is down (before sunrise or after sunset)
// NOAA algorithm for sunrise/sunset calculation
bool isSunDown() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  
  int year = timeinfo->tm_year + 1900;
  int month = timeinfo->tm_mon + 1;
  int day = timeinfo->tm_mday;
  int hour = timeinfo->tm_hour;
  int minute = timeinfo->tm_min;
  
  // Calculate day of year
  int N1 = floor(275 * month / 9);
  int N2 = floor((month + 9) / 12);
  int N3 = (1 + floor((year - 4 * floor(year / 4) + 2) / 3));
  int dayOfYear = N1 - (N2 * N3) + day;
  
  // Convert to fractional day (UTC+1 for Oslo)
  double fractionalDay = dayOfYear + (hour - 1) / 24.0 + minute / 1440.0;
  
  // Mean solar time
  double J2000 = 2451545.0;
  double JD = fractionalDay + 2451544.5;
  double n = JD - J2000;
  
  // Solar mean longitude
  double L = fmod(280.46646 + 0.9856474 * n, 360.0);
  if (L < 0) L += 360.0;
  
  // Mean anomaly
  double g = fmod(357.52911 + 0.9856473 * n, 360.0);
  if (g < 0) g += 360.0;
  
  // Convert to radians
  double g_rad = g * M_PI / 180.0;
  
  // Solar equation of center
  double C = (1.914602 - 0.004817 * n / 36525 - 0.000014 * n / 36525 * n / 36525) * sin(g_rad);
  C += (0.019993 - 0.000101 * n / 36525) * sin(2 * g_rad);
  C += 0.000029 * sin(3 * g_rad);
  
  // Solar true longitude
  double sunLong = L + C;
  
  // Solar apparent longitude
  double Omega = fmod(125.04 - 1.9 * n / 36525, 360.0);
  double lambda = sunLong - 0.00569 - 0.00478 * sin(Omega * M_PI / 180.0);
  
  // Solar declination
  double epsilon = 23.439291 - 0.0130042 * n / 36525 - 0.00000164 * n / 36525 * n / 36525;
  double lambda_rad = lambda * M_PI / 180.0;
  double epsilon_rad = epsilon * M_PI / 180.0;
  double delta = asin(sin(epsilon_rad) * sin(lambda_rad)) * 180.0 / M_PI;
  
  // Observer's latitude and longitude (Oslo)
  double lat = 59.9139;
  double lon = 10.7522;
  double lat_rad = lat * M_PI / 180.0;
  double delta_rad = delta * M_PI / 180.0;
  
  // Hour angle
  double H0_rad = acos(-tan(lat_rad) * tan(delta_rad));
  if (isnan(H0_rad)) {
    // Sun always above or below horizon
    return sin(lat_rad) * sin(delta_rad) < 0; // true if always below
  }
  
  double H0 = H0_rad * 180.0 / M_PI;
  
  // Equation of time
  double eot = 229.18 * (0.016708617 - n / 36525 * (0.042037 + 0.0001267 * n / 36525)) * sin(g_rad);
  eot -= 2.4 * cos(2 * g_rad) + 4.0 * sin(2 * g_rad);
  
  // Sunrise and sunset (in local time, UTC+1)
  double sunrise = 12 - H0 / 15 - lon / 15 - 1; // -1 for UTC offset
  double sunset = 12 + H0 / 15 - lon / 15 - 1;
  
  double currentTime = hour + minute / 60.0;
  
  bool dark = (currentTime < sunrise) || (currentTime > sunset);
  return dark;
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
    cfg.voltage_divider = 24.0; // default multiplier; adjust in portal
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

  topic_firmware_cmd = topic_base + "/firmware/set";
  topic_firmware_state = topic_base + "/firmware";
  topic_power_cmd = topic_base + "/power";
  topic_down_cmd = topic_base + "/down";
  topic_up_cmd = topic_base + "/up";
  topic_mode_cmd = topic_base + "/mode";
  topic_status = topic_base + "/status";

  mqttClient.subscribe(topic_relay1_cmd.c_str());
  mqttClient.subscribe(topic_relay2_cmd.c_str());
  mqttClient.subscribe(topic_relay3_cmd.c_str());
  mqttClient.subscribe(topic_relay4_cmd.c_str());
  mqttClient.subscribe(topic_precondition_cmd.c_str());
  mqttClient.subscribe(topic_auto_low_cmd.c_str());
  mqttClient.subscribe((topic_base + "/settings/set").c_str());
  mqttClient.subscribe(topic_firmware_cmd.c_str());
  mqttClient.subscribe(LEGACY_FIRMWARE_CMD);
  mqttClient.subscribe(topic_power_cmd.c_str());
  mqttClient.subscribe(topic_down_cmd.c_str());
  mqttClient.subscribe(topic_up_cmd.c_str());
  mqttClient.subscribe(topic_mode_cmd.c_str());
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  Serial.printf("[OTA DEBUG] Message received on topic: %s (len=%u)\n", topic, length);
  Serial.printf("[OTA DEBUG] topic_firmware_cmd=%s, LEGACY_FIRMWARE_CMD=%s\n", topic_firmware_cmd.c_str(), LEGACY_FIRMWARE_CMD);
  Serial.printf("[OTA DEBUG] Topic match: firmware_cmd=%d, legacy=%d\n", 
                (t == topic_firmware_cmd) ? 1 : 0, 
                (t == String(LEGACY_FIRMWARE_CMD)) ? 1 : 0);

  // Handle OTA firmware update command
  if (t == topic_firmware_cmd || t == String(LEGACY_FIRMWARE_CMD)) {
    Serial.printf("[OTA DEBUG] OTA topic matched! Message: %s\n", msg.c_str());
    // Extract URL from message (format: "self_update <url>" or just URL)
    String url = msg;
    if (msg.startsWith("self_update ")) {
      url = msg.substring(12); // skip "self_update "
    }
    if (url.length() > 10 && (url.startsWith("http://") || url.startsWith("https://"))) {
      Serial.printf("OTA command received with URL: %s\n", url.c_str());
      performSelfUpdate(url);
    } else if (msg.equalsIgnoreCase("self_update")) {
      // Use default configured URL
      Serial.printf("OTA command received, using default URL: %s\n", FIRMWARE_URL);
      performSelfUpdate(String(FIRMWARE_URL));
    }
    return;
  }

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
  
  // Relay3 (ILLUMINATION) - manual brightness override via MQTT
  if (t == topic_relay3_cmd) {
    bool newState = (msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true"));
    relay3_state = newState;
    setRelay(RELAY3_PIN, relay3_state);
    publishState(topic_relay3_state, relay3_state);
  }
  
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
    // Publish firmware info
    publishFirmwareInfo();
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
    mqttClient.publish(disc.c_str(), buf, true);
  };

  publishSwitch("acc", "ACC", topic_relay4_cmd, topic_relay4_state);
  publishSwitch("ready", "READY", topic_relay2_cmd, topic_relay2_state);
  publishSwitch("illumination", "Illumination", topic_relay3_cmd, topic_relay3_state);
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
  mqttClient.publish(disc_batt.c_str(), buf2, true);

  // firmware sensor
  String disc_fw = String("homeassistant/sensor/") + node + "/firmware/config";
  StaticJsonDocument<256> doc3;
  doc3["name"] = "Heater Firmware";
  doc3["uniq_id"] = node + String("_firmware");
  doc3["stat_t"] = topic_firmware_state;
  doc3["val_tpl"] = "{{ value_json.fw_version ~ ' (' ~ value_json.build_time ~ ')' }}";
  doc3["icon"] = "mdi:chip";
  char buf3[256]; serializeJson(doc3, buf3);
  mqttClient.publish(disc_fw.c_str(), buf3, true);

  // self update button
  String disc_btn = String("homeassistant/button/") + node + "/self_update/config";
  StaticJsonDocument<384> doc4;
  doc4["name"] = "Heater Self Update";
  doc4["uniq_id"] = node + String("_self_update");
  doc4["cmd_t"] = topic_firmware_cmd;
  doc4["pl_prs"] = String("self_update ") + FIRMWARE_URL;
  doc4["icon"] = "mdi:update";
  char buf4[384]; serializeJson(doc4, buf4);
  mqttClient.publish(disc_btn.c_str(), buf4, true);
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

  // Configure static IP: 192.168.1.101
  IPAddress staticIP(192, 168, 1, 101);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns1(192, 168, 1, 1);     // Router as DNS
  IPAddress dns2(8, 8, 8, 8);         // Google DNS fallback
  
  WiFi.config(staticIP, gateway, subnet, dns1, dns2);

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

  // Synchronize time with NTP server (needed for sun calculations)
  configTime(1 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // UTC+1 for Oslo (winter), will handle DST via system
  Serial.println("Waiting for NTP time sync...");
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 30) {
    delay(500);
    now = time(nullptr);
    attempts++;
  }
  Serial.printf("Time synced: %s\n", ctime(&now));

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

  // Sun-based automation for LIGHT relay (check every minute)
  if (sun_automation_enabled && millis() - last_sun_check > SUN_CHECK_INTERVAL) {
    last_sun_check = millis();
    bool was_dark = is_dark_now;
    is_dark_now = isSunDown();
    
    // Change relay state if darkness changed
    if (is_dark_now != was_dark) {
      // LIGHT relay
      relay1_state = is_dark_now; // Turn ON when dark, OFF when light
      setRelay(RELAY1_PIN, relay1_state);
      publishState(topic_relay1_state, relay1_state);
      // ILLUMINATION relay: dim display when dark (illumination wire ON = dim mode)
      relay3_state = is_dark_now;
      setRelay(RELAY3_PIN, relay3_state);
      publishState(topic_relay3_state, relay3_state);
      Serial.printf("Sun automation: Light=%s, Illumination=%s (sun is %s)\n",
                    relay1_state ? "ON" : "OFF",
                    relay3_state ? "DIM" : "BRIGHT",
                    is_dark_now ? "down" : "up");
    }
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