#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include <FirmwareOTA.h>
#include <time.h>
#include <cmath>

const char* FIRMWARE_VERSION = "1.2";
const char* FIRMWARE_BUILD_TIME = __DATE__ " " __TIME__;
const char* FIRMWARE_URL = "https://github.com/tarcsajana/Heater/releases/latest/download/firmware.bin";

#ifndef GITHUB_TOKEN
#define GITHUB_TOKEN ""
#endif

const char* OTA_GITHUB_TOKEN = GITHUB_TOKEN;

// Default ESP32-C3 pin mapping. Adjust these to match the relay board wiring.
const uint8_t RELAY1_PIN = 3;  // LIGHT (inverted)
const uint8_t RELAY4_PIN = 4;  // ACC (inverted)
const uint8_t RELAY2_PIN = 5;  // READY (inverted)
const uint8_t RELAY3_PIN = 6;  // ILLUMINATION (inverted)
const uint8_t ADC_PIN = 0;

const double OSLO_LATITUDE = 59.9139;
const double OSLO_LONGITUDE = 10.7522;
bool sun_automation_enabled = true;
bool is_dark_now = false;
unsigned long last_sun_check = 0;
const unsigned long SUN_CHECK_INTERVAL = 60000UL;

const char* MQTT_HOST = "kebnekaise.lmq.cloudamqp.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = "lpomtbnx:lpomtbnx";
const char* MQTT_PASS = "9htYKaGbPej2YOYyHYGZEbkWdpDdT0Mv";
const char* DEFAULT_BASE_TOPIC = "heater";
const char* DEFAULT_WIFI_SSID = "HunorCar";
const char* DEFAULT_WIFI_PASS = "Jucymucy8581";
const char* DEVICE_ID = "heater_esp32c3";
const char* DEVICE_NAME = "Heater Controller";
const char* LEGACY_FIRMWARE_CMD = "heater/firmware/set";

struct Config {
  float voltage_divider;
  float low_voltage_threshold;
  uint8_t auto_low_enabled;
  uint32_t battery_interval_s;
  float battery_delta;
  uint32_t magic;
};

Config cfg;
const uint32_t CONFIG_MAGIC = 0xDEADBEEF;

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer webServer(80);
WiFiManager wifiManager;
FirmwareOTA ota(webServer, mqttClient, DEVICE_ID, DEVICE_NAME);

WiFiManagerParameter* p_custom_vdiv = nullptr;
WiFiManagerParameter* p_custom_lowv = nullptr;
WiFiManagerParameter* p_custom_autolow = nullptr;
WiFiManagerParameter* p_custom_batint = nullptr;
WiFiManagerParameter* p_custom_batdelt = nullptr;

bool relay1_state = false;
bool relay2_state = false;
bool relay3_state = false;
bool relay4_state = false;
bool precondition_state = false;
bool portal_requested = false;

unsigned long precond_on_until = 0;
unsigned long seq_step_until = 0;
int seq_step = 0;

String topic_base;
String topic_relay1_cmd;
String topic_relay1_state;
String topic_relay2_cmd;
String topic_relay2_state;
String topic_relay3_cmd;
String topic_relay3_state;
String topic_relay4_cmd;
String topic_relay4_state;
String topic_precondition_cmd;
String topic_precondition_state;
String topic_battery_state;
String topic_auto_low_cmd;
String topic_auto_low_state;
String topic_settings_cmd;
String topic_firmware_cmd;
String topic_firmware_state;
String topic_command;
String topic_power_cmd;
String topic_down_cmd;
String topic_up_cmd;
String topic_mode_cmd;
String topic_status;

void setRelay(uint8_t pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH);
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
    memset(&cfg, 0, sizeof(cfg));
    cfg.voltage_divider = 24.0f;
    cfg.low_voltage_threshold = 10.5f;
    cfg.auto_low_enabled = 0;
    cfg.battery_interval_s = 60;
    cfg.battery_delta = 0.10f;
    cfg.magic = CONFIG_MAGIC;
    saveConfig();
  }
}

void publishState(const String& topic, bool state) {
  if (mqttClient.connected()) {
    mqttClient.publish(topic.c_str(), state ? "ON" : "OFF", true);
  }
}

float readBatteryVoltage() {
  const float adcFullScaleVoltage = 1.1f;
  int raw = analogRead(ADC_PIN);
  float adcVoltage = (static_cast<float>(raw) / 4095.0f) * adcFullScaleVoltage;
  return adcVoltage * cfg.voltage_divider;
}

bool isSunDown() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int year = timeinfo.tm_year + 1900;
  int month = timeinfo.tm_mon + 1;
  int day = timeinfo.tm_mday;
  int hour = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;

  int n1 = static_cast<int>(floor(275.0 * month / 9.0));
  int n2 = static_cast<int>(floor((month + 9.0) / 12.0));
  int n3 = 1 + static_cast<int>(floor((year - 4.0 * floor(year / 4.0) + 2.0) / 3.0));
  int dayOfYear = n1 - (n2 * n3) + day;

  double fractionalDay = dayOfYear + (hour - 1) / 24.0 + minute / 1440.0;
  double j2000 = 2451545.0;
  double jd = fractionalDay + 2451544.5;
  double n = jd - j2000;

  double l = fmod(280.46646 + 0.9856474 * n, 360.0);
  if (l < 0) {
    l += 360.0;
  }

  double g = fmod(357.52911 + 0.9856473 * n, 360.0);
  if (g < 0) {
    g += 360.0;
  }

  double gRad = g * M_PI / 180.0;
  double c = (1.914602 - 0.004817 * n / 36525.0 - 0.000014 * n / 36525.0 * n / 36525.0) * sin(gRad);
  c += (0.019993 - 0.000101 * n / 36525.0) * sin(2.0 * gRad);
  c += 0.000029 * sin(3.0 * gRad);

  double sunLong = l + c;
  double omega = fmod(125.04 - 1.9 * n / 36525.0, 360.0);
  double lambda = sunLong - 0.00569 - 0.00478 * sin(omega * M_PI / 180.0);

  double epsilon = 23.439291 - 0.0130042 * n / 36525.0 - 0.00000164 * n / 36525.0 * n / 36525.0;
  double lambdaRad = lambda * M_PI / 180.0;
  double epsilonRad = epsilon * M_PI / 180.0;
  double delta = asin(sin(epsilonRad) * sin(lambdaRad)) * 180.0 / M_PI;

  double latRad = OSLO_LATITUDE * M_PI / 180.0;
  double deltaRad = delta * M_PI / 180.0;
  double h0Rad = acos(-tan(latRad) * tan(deltaRad));
  if (isnan(h0Rad)) {
    return sin(latRad) * sin(deltaRad) < 0;
  }

  double h0 = h0Rad * 180.0 / M_PI;
  double sunrise = 12.0 - h0 / 15.0 - OSLO_LONGITUDE / 15.0 - 1.0;
  double sunset = 12.0 + h0 / 15.0 - OSLO_LONGITUDE / 15.0 - 1.0;
  double currentTime = hour + minute / 60.0;
  return currentTime < sunrise || currentTime > sunset;
}

void publishHADiscovery();

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
  topic_settings_cmd = topic_base + "/settings/set";
  topic_firmware_cmd = topic_base + "/firmware/set";
  topic_firmware_state = topic_base + "/firmware";
  topic_command = topic_base + "/command";
  topic_power_cmd = topic_base + "/power";
  topic_down_cmd = topic_base + "/down";
  topic_up_cmd = topic_base + "/up";
  topic_mode_cmd = topic_base + "/mode";
  topic_status = topic_base + "/status";

  ota.setMqttTopics(topic_firmware_state.c_str(), topic_command.c_str(), topic_status.c_str());
}

void subscribeTopics() {
  mqttClient.subscribe(topic_relay1_cmd.c_str());
  mqttClient.subscribe(topic_relay2_cmd.c_str());
  mqttClient.subscribe(topic_relay3_cmd.c_str());
  mqttClient.subscribe(topic_relay4_cmd.c_str());
  mqttClient.subscribe(topic_precondition_cmd.c_str());
  mqttClient.subscribe(topic_auto_low_cmd.c_str());
  mqttClient.subscribe(topic_settings_cmd.c_str());
  mqttClient.subscribe(topic_command.c_str());
  mqttClient.subscribe(topic_firmware_cmd.c_str());
  mqttClient.subscribe(LEGACY_FIRMWARE_CMD);
  mqttClient.subscribe(topic_power_cmd.c_str());
  mqttClient.subscribe(topic_down_cmd.c_str());
  mqttClient.subscribe(topic_up_cmd.c_str());
  mqttClient.subscribe(topic_mode_cmd.c_str());
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += static_cast<char>(payload[i]);
  }
  msg.trim();

  if (ota.handleMqttUpdate(topic, msg)) {
    return;
  }

  if (t == topic_firmware_cmd || t == String(LEGACY_FIRMWARE_CMD)) {
    String normalized = msg;
    if (normalized.startsWith("http://") || normalized.startsWith("https://")) {
      normalized = String("self_update ") + normalized;
    }
    if (normalized.equalsIgnoreCase("self_update") || normalized.startsWith("self_update ")) {
      ota.handleMqttUpdate(topic_command.c_str(), normalized);
    }
    return;
  }

  auto handleToggle = [&](const String& commandTopic, bool& state, uint8_t pin, const String& stateTopic) {
    if (t != commandTopic) {
      return;
    }

    bool newState = msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true");
    state = newState;
    setRelay(pin, state);
    publishState(stateTopic, state);
  };

  handleToggle(topic_relay1_cmd, relay1_state, RELAY1_PIN, topic_relay1_state);
  handleToggle(topic_relay2_cmd, relay2_state, RELAY2_PIN, topic_relay2_state);
  handleToggle(topic_relay4_cmd, relay4_state, RELAY4_PIN, topic_relay4_state);

  if (t == topic_relay3_cmd) {
    relay3_state = msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true");
    setRelay(RELAY3_PIN, relay3_state);
    publishState(topic_relay3_state, relay3_state);
  }

  if (t == topic_precondition_cmd) {
    bool newState = msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true");
    if (newState && !precondition_state) {
      precondition_state = true;
      publishState(topic_precondition_state, true);
      relay4_state = true;
      setRelay(RELAY4_PIN, true);
      publishState(topic_relay4_state, true);
      seq_step = 1;
      seq_step_until = millis() + 3000UL;
    } else if (!newState && precondition_state) {
      precondition_state = false;
      relay4_state = false;
      setRelay(RELAY4_PIN, false);
      publishState(topic_relay4_state, false);
      publishState(topic_precondition_state, false);
      seq_step = 0;
      seq_step_until = 0;
      precond_on_until = 0;
    }
  }

  if (t == topic_auto_low_cmd) {
    cfg.auto_low_enabled = (msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true")) ? 1 : 0;
    saveConfig();
    if (mqttClient.connected()) {
      mqttClient.publish(topic_auto_low_state.c_str(), cfg.auto_low_enabled ? "ON" : "OFF", true);
    }
  }

  if (t == topic_settings_cmd) {
    bool enablePortal = msg.equalsIgnoreCase("on") || msg == "1" || msg.equalsIgnoreCase("true");
    if (enablePortal) {
      portal_requested = true;
      Serial.println("Settings portal requested via MQTT");
    }
  }
}

void syncTimeFromNtp() {
  configTime(3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("Waiting for NTP time sync...");

  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 24 * 3600 && attempts < 30) {
    delay(500);
    now = time(nullptr);
    attempts++;
  }

  if (now >= 24 * 3600) {
    Serial.printf("Time synced: %s", ctime(&now));
  } else {
    Serial.println("Time sync not completed yet");
  }
}

void mqttReconnect() {
  static unsigned long lastAttempt = 0;
  static bool haDiscoveryPublished = false;

  if (mqttClient.connected()) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (millis() - lastAttempt < 5000UL) {
    return;
  }

  lastAttempt = millis();
  setupTopics();

  String nodeId = WiFi.macAddress();
  nodeId.replace(":", "");
  String clientId = String("heater-") + nodeId;

  mqttClient.setSocketTimeout(5);

  bool connected = mqttClient.connect(
    clientId.c_str(),
    MQTT_USER,
    MQTT_PASS,
    topic_status.c_str(),
    0,
    true,
    "offline"
  );

  if (!connected) {
    Serial.printf("MQTT connect failed: %d\n", mqttClient.state());
    return;
  }

  subscribeTopics();
  mqttClient.publish(topic_status.c_str(), "online", true);
  publishState(topic_relay1_state, relay1_state);
  publishState(topic_relay2_state, relay2_state);
  publishState(topic_relay3_state, relay3_state);
  publishState(topic_relay4_state, relay4_state);
  publishState(topic_precondition_state, precondition_state);
  mqttClient.publish(topic_auto_low_state.c_str(), cfg.auto_low_enabled ? "ON" : "OFF", true);
  ota.publishFirmwareInfo();

  if (!haDiscoveryPublished) {
    publishHADiscovery();
    haDiscoveryPublished = true;
  }
}

void publishHADiscovery() {
  if (!mqttClient.connected()) {
    return;
  }

  String node = WiFi.macAddress();
  node.replace(":", "");
  String haDeviceId = String("heater_") + node;
  String haDeviceName = String("Heater Controller ") + node;
  StaticJsonDocument<768> doc;
  char buf[768];

  auto publishSwitch = [&](const String& objectId, const String& name, const String& commandTopic, const String& stateTopic) {
    String discoveryTopic = String("homeassistant/switch/") + node + "/" + objectId + "/config";
    doc.clear();
    doc["name"] = name;
    doc["uniq_id"] = node + "_" + objectId;
    doc["cmd_t"] = commandTopic;
    doc["stat_t"] = stateTopic;
    doc["pl_on"] = "ON";
    doc["pl_off"] = "OFF";

    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = haDeviceId;
    device["name"] = haDeviceName;
    device["manufacturer"] = "Custom";
    device["model"] = "Heater Controller ESP32-C3";
    device["sw_version"] = FIRMWARE_VERSION;

    serializeJson(doc, buf);
    mqttClient.publish(discoveryTopic.c_str(), buf, true);
    yield();
  };

  publishSwitch("acc", "ACC", topic_relay4_cmd, topic_relay4_state);
  publishSwitch("ready", "READY", topic_relay2_cmd, topic_relay2_state);
  publishSwitch("illumination", "Illumination", topic_relay3_cmd, topic_relay3_state);
  publishSwitch("light", "LIGHT", topic_relay1_cmd, topic_relay1_state);
  publishSwitch("precondition", "PRECONDITION", topic_precondition_cmd, topic_precondition_state);
  publishSwitch("auto_low", "Auto PRECOND Low", topic_auto_low_cmd, topic_auto_low_state);

  String batteryDiscoveryTopic = String("homeassistant/sensor/") + node + "/battery/config";
  doc.clear();
  doc["name"] = "Heater Battery";
  doc["uniq_id"] = node + String("_battery");
  doc["stat_t"] = topic_battery_state;
  doc["unit_of_meas"] = "V";
  doc["device_class"] = "voltage";
  JsonObject batteryDevice = doc["device"].to<JsonObject>();
  batteryDevice["identifiers"][0] = haDeviceId;
  batteryDevice["name"] = haDeviceName;
  batteryDevice["manufacturer"] = "Custom";
  batteryDevice["model"] = "Heater Controller ESP32-C3";
  batteryDevice["sw_version"] = FIRMWARE_VERSION;
  serializeJson(doc, buf);
  mqttClient.publish(batteryDiscoveryTopic.c_str(), buf, true);
  yield();

  String firmwareDiscoveryTopic = String("homeassistant/sensor/") + node + "/firmware/config";
  doc.clear();
  doc["name"] = "Heater Firmware";
  doc["uniq_id"] = node + String("_firmware");
  doc["stat_t"] = topic_firmware_state;
  doc["val_tpl"] = "{{ value_json.fw_version ~ ' (' ~ value_json.build_time ~ ')' }}";
  doc["icon"] = "mdi:chip";
  JsonObject firmwareDevice = doc["device"].to<JsonObject>();
  firmwareDevice["identifiers"][0] = haDeviceId;
  firmwareDevice["name"] = haDeviceName;
  firmwareDevice["manufacturer"] = "Custom";
  firmwareDevice["model"] = "Heater Controller ESP32-C3";
  firmwareDevice["sw_version"] = FIRMWARE_VERSION;
  serializeJson(doc, buf);
  mqttClient.publish(firmwareDiscoveryTopic.c_str(), buf, true);
  yield();

  String updateDiscoveryTopic = String("homeassistant/button/") + node + "/self_update/config";
  doc.clear();
  doc["name"] = "Heater Self Update";
  doc["uniq_id"] = node + String("_self_update");
  doc["cmd_t"] = topic_command;
  doc["pl_prs"] = "self_update";
  doc["icon"] = "mdi:update";
  JsonObject updateDevice = doc["device"].to<JsonObject>();
  updateDevice["identifiers"][0] = haDeviceId;
  updateDevice["name"] = haDeviceName;
  updateDevice["manufacturer"] = "Custom";
  updateDevice["model"] = "Heater Controller ESP32-C3";
  updateDevice["sw_version"] = FIRMWARE_VERSION;
  serializeJson(doc, buf);
  mqttClient.publish(updateDiscoveryTopic.c_str(), buf, true);
}

void handlePortalRequest() {
  portal_requested = true;
  webServer.send(200, "text/plain", "Settings portal will start in a few seconds. Connect to 'Heater-Setup-AP' WiFi network.");
}

void handleRoot() {
  String html = "<html><head><title>Heater Settings</title></head><body>";
  html += "<h1>Heater Control</h1>";
  html += "<p><a href='/settings'>Open Settings Portal</a></p>";
  html += "<p>Device IP: " + WiFi.localIP().toString() + "</p>";
  html += "</body></html>";
  webServer.send(200, "text/html", html);
}

void startSettingsPortal() {
  Serial.println("Starting settings portal...");
  webServer.stop();
  wifiManager.setConfigPortalTimeout(0);
  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }

  bool result = wifiManager.startConfigPortal("Heater-Setup-AP", DEFAULT_WIFI_PASS);
  Serial.printf("Config portal finished, result=%d\n", result ? 1 : 0);

  if (p_custom_vdiv) {
    cfg.voltage_divider = atof(p_custom_vdiv->getValue());
  }
  if (p_custom_lowv) {
    cfg.low_voltage_threshold = atof(p_custom_lowv->getValue());
  }
  if (p_custom_autolow) {
    cfg.auto_low_enabled = static_cast<uint8_t>(atoi(p_custom_autolow->getValue()));
  }
  if (p_custom_batint) {
    cfg.battery_interval_s = static_cast<uint32_t>(atoi(p_custom_batint->getValue()));
  }
  if (p_custom_batdelt) {
    cfg.battery_delta = atof(p_custom_batdelt->getValue());
  }
  saveConfig();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  syncTimeFromNtp();
  setupTopics();
  webServer.begin();
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
  setRelay(RELAY1_PIN, false);
  setRelay(RELAY2_PIN, false);
  setRelay(RELAY3_PIN, false);
  setRelay(RELAY4_PIN, false);

  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_0db);

  loadConfig();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  IPAddress staticIP(192, 168, 1, 101);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  IPAddress dns1(192, 168, 1, 1);
  IPAddress dns2(8, 8, 8, 8);
  WiFi.config(staticIP, gateway, subnet, dns1, dns2);

  wifiManager.setSaveConfigCallback([]() {});

  char vdbuf[16];
  dtostrf(cfg.voltage_divider, 4, 2, vdbuf);
  char lowvbuf[8];
  dtostrf(cfg.low_voltage_threshold, 4, 2, lowvbuf);
  char autolowbuf[4];
  snprintf(autolowbuf, sizeof(autolowbuf), "%u", cfg.auto_low_enabled);
  char batintbuf[8];
  snprintf(batintbuf, sizeof(batintbuf), "%u", static_cast<unsigned>(cfg.battery_interval_s));
  char batdeltbuf[8];
  dtostrf(cfg.battery_delta, 4, 2, batdeltbuf);

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

  if (strlen(DEFAULT_WIFI_SSID) > 0) {
    Serial.printf("Trying default WiFi '%s'...\n", DEFAULT_WIFI_SSID);
    WiFi.begin(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000UL) {
      delay(500);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    wifiManager.autoConnect("Heater-Setup-AP", DEFAULT_WIFI_PASS);
    WiFi.setSleep(false);
  }

  if (p_custom_vdiv) {
    cfg.voltage_divider = atof(p_custom_vdiv->getValue());
  }
  if (p_custom_lowv) {
    cfg.low_voltage_threshold = atof(p_custom_lowv->getValue());
  }
  if (p_custom_autolow) {
    cfg.auto_low_enabled = static_cast<uint8_t>(atoi(p_custom_autolow->getValue()));
  }
  if (p_custom_batint) {
    cfg.battery_interval_s = static_cast<uint32_t>(atoi(p_custom_batint->getValue()));
  }
  if (p_custom_batdelt) {
    cfg.battery_delta = atof(p_custom_batdelt->getValue());
  }
  saveConfig();

  setupTopics();

  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(30);
  mqttClient.setSocketTimeout(10);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);

  ota.setVersionInfo(FIRMWARE_VERSION, FIRMWARE_BUILD_TIME);
  ota.setFirmwareUrl(FIRMWARE_URL);
  if (strlen(OTA_GITHUB_TOKEN) > 0) {
    ota.setGitHubToken(OTA_GITHUB_TOKEN);
  }

  webServer.on("/", handleRoot);
  webServer.on("/settings", handlePortalRequest);
  ota.setupWebRoutes();
  webServer.begin();

  syncTimeFromNtp();

  Serial.println("Web server started on port 80");
  Serial.printf("Access via http://%s\n", WiFi.localIP().toString().c_str());
}

void loop() {
  static unsigned long lastWiFiCheck = 0;

  webServer.handleClient();

  if (portal_requested) {
    portal_requested = false;
    delay(1000);
    startSettingsPortal();
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWiFiCheck >= 10000UL) {
      lastWiFiCheck = millis();
      Serial.println("WiFi disconnected, retrying...");
      WiFi.reconnect();
    }
  } else {
    mqttReconnect();
    mqttClient.loop();
  }

  if (seq_step == 1 && millis() >= seq_step_until) {
    relay2_state = true;
    setRelay(RELAY2_PIN, true);
    publishState(topic_relay2_state, true);
    seq_step = 2;
    seq_step_until = millis() + 2000UL;
  } else if (seq_step == 2 && millis() >= seq_step_until) {
    relay2_state = false;
    setRelay(RELAY2_PIN, false);
    publishState(topic_relay2_state, false);
    seq_step = 0;
    seq_step_until = 0;
  }

  if (precond_on_until != 0 && millis() > precond_on_until) {
    precondition_state = false;
    precond_on_until = 0;
    relay4_state = false;
    setRelay(RELAY4_PIN, false);
    publishState(topic_relay4_state, false);
    publishState(topic_precondition_state, false);
  }

  if (sun_automation_enabled && millis() - last_sun_check > SUN_CHECK_INTERVAL) {
    last_sun_check = millis();
    bool wasDark = is_dark_now;
    is_dark_now = isSunDown();

    if (is_dark_now != wasDark) {
      relay1_state = is_dark_now;
      setRelay(RELAY1_PIN, relay1_state);
      publishState(topic_relay1_state, relay1_state);

      relay3_state = is_dark_now;
      setRelay(RELAY3_PIN, relay3_state);
      publishState(topic_relay3_state, relay3_state);

      Serial.printf(
        "Sun automation: Light=%s, Illumination=%s (sun is %s)\n",
        relay1_state ? "ON" : "OFF",
        relay3_state ? "DIM" : "BRIGHT",
        is_dark_now ? "down" : "up"
      );
    }
  }

  static unsigned long lastBatteryPublish = 0;
  static float lastPublishedBattery = 0.0f;
  static bool firstBatteryPublish = true;
  unsigned long batteryIntervalMs = cfg.battery_interval_s > 0 ? cfg.battery_interval_s * 1000UL : 60000UL;

  if (millis() - lastBatteryPublish > batteryIntervalMs) {
    lastBatteryPublish = millis();
    float batteryVoltage = readBatteryVoltage();
    float delta = fabsf(batteryVoltage - lastPublishedBattery);
    bool shouldPublish = firstBatteryPublish || delta >= cfg.battery_delta;

    if (shouldPublish && mqttClient.connected()) {
      char buf[16];
      dtostrf(batteryVoltage, 4, 2, buf);
      mqttClient.publish(topic_battery_state.c_str(), buf, true);
      lastPublishedBattery = batteryVoltage;
      firstBatteryPublish = false;
    }

    if (cfg.auto_low_enabled && batteryVoltage < cfg.low_voltage_threshold && !precondition_state) {
      precondition_state = true;
      publishState(topic_precondition_state, true);
      relay4_state = true;
      setRelay(RELAY4_PIN, true);
      publishState(topic_relay4_state, true);
      seq_step = 1;
      seq_step_until = millis() + 3000UL;
      precond_on_until = millis() + 20UL * 60UL * 1000UL;
    }
  }
}