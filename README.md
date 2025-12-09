Heater (Wemos D1 mini) MQTT firmware

Summary
- ESP8266 (Wemos D1 mini) sketch using PlatformIO
- WiFi configuration portal (WiFiManager) with custom fields for MQTT and calibration
- MQTT via `PubSubClient` and Home Assistant MQTT Discovery
- Inverted relay outputs on specified D pins, and analog battery reading

Pinout
- `D8` -> Relay1 -> LIGHT (inverted)
- `D5` -> Relay4 -> ACC (inverted)
- `D7` -> Relay2 -> READY (inverted)
- `D6` -> Relay3 -> HORN (inverted)
- `A0` -> battery ADC (use `voltage multiplier` to calibrate)

MQTT Topics (default base topic `heater`)
- `<base>/<mac>/relay1/set` (command)
- `<base>/<mac>/relay1/state` (state)
- `<base>/<mac>/relay2/set`
- `<base>/<mac>/relay2/state`
- `<base>/<mac>/relay3/set`
- `<base>/<mac>/relay3/state`
- `<base>/<mac>/relay4/set`
- `<base>/<mac>/relay4/state`
- `<base>/<mac>/precondition/set` (precondition on/off)
- `<base>/<mac>/precondition/state`
- `<base>/<mac>/battery` (battery voltage, V)

Home Assistant
- The firmware publishes MQTT discovery messages under `homeassistant/...` so entities should appear automatically.
- Relay mapping per your request:
  - `relay4` -> ACC
  - `relay2` -> READY
  - `relay3` -> HORN
  - `relay1` -> LIGHT
  - `precondition` -> PRECONDITION switch

Precondition behavior
- Turning PRECONDITION ON via MQTT/Home Assistant triggers:
  - ACC ON immediately
  - after 3 seconds: READY ON
  - after 2 seconds: READY OFF
- Turning PRECONDITION OFF turns ACC OFF immediately and cancels pending sequences
- If battery voltage falls below the configured `Low Voltage Threshold` (default `10.5V`), PRECONDITION will be enabled automatically for 20 minutes.

Setup
1. Build and upload with PlatformIO (`env:wemos8266`).
2. On first boot, the device will start an AP `Heater-Setup-AP`. Connect and configure WiFi and MQTT server credentials and calibration values.
3. After connecting to WiFi, the device will connect to MQTT and publish Home Assistant discovery config.

Calibration
- The field `Voltage Multiplier` in the portal multiplies the measured ADC voltage to the real battery voltage. Default is `12.00` which is a reasonable starting point for 12V batteries; calibrate with a multimeter.

Notes
- The sketch stores configuration in EEPROM.
- Adjust `voltage_divider` if readings are incorrect.

If you'd like, I can:
- add OTA updates
- publish logs to MQTT
- tweak ADC scaling to match your exact resistor divider once you tell me the resistor values

