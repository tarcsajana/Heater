Heater ESP32-C3 MQTT firmware

Summary
- ESP32-C3 sketch using PlatformIO
- WiFi configuration portal (WiFiManager) for WiFi plus battery calibration values
- MQTT via `PubSubClient`, Home Assistant MQTT Discovery, and FirmwareOTA integration
- Inverted relay outputs on GPIO pins, and analog battery reading

Pinout
- `GPIO3` -> Relay1 -> LIGHT (inverted)
- `GPIO4` -> Relay4 -> ACC (inverted)
- `GPIO5` -> Relay2 -> READY (inverted)
- `GPIO6` -> Relay3 -> ILLUMINATION (inverted)
- `GPIO0` -> battery ADC (use `voltage multiplier` to calibrate)
- Adjust the GPIO constants in `src/main.cpp` if your ESP32-C3 board is wired differently.

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
- `<base>/<mac>/command` (FirmwareOTA command topic, payload `self_update`)
- `<base>/<mac>/firmware` (firmware metadata topic)
- `<base>/<mac>/status` (availability topic)

Home Assistant
- The firmware publishes MQTT discovery messages under `homeassistant/...` so entities should appear automatically.
- OTA now follows the same command/firmware/status topic pattern used by CanBridge.
- Relay mapping per your request:
  - `relay4` -> ACC
  - `relay2` -> READY
  - `relay3` -> ILLUMINATION
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
1. Build and upload with PlatformIO (`env:esp32c3`).
2. On first boot, the device will start an AP `Heater-Setup-AP`. Connect and configure WiFi and MQTT server credentials and calibration values.
3. After connecting to WiFi, the device will connect to MQTT and publish Home Assistant discovery config.
4. OTA updates can be triggered over MQTT by publishing `self_update` to `<base>/<mac>/command`, matching the CanBridge OTA command flow.

Calibration
- The field `Voltage Multiplier` in the portal multiplies the measured ADC voltage to the real battery voltage. Default is `12.00` which is a reasonable starting point for 12V batteries; calibrate with a multimeter.

Notes
- The sketch stores configuration in EEPROM.
- Adjust `voltage_divider` if readings are incorrect.
- ESP32-C3 ADC scaling differs from ESP8266, so expect to recalibrate the battery reading after moving the hardware.

If you'd like, I can:
- add OTA updates
- publish logs to MQTT
- tweak ADC scaling to match your exact resistor divider once you tell me the resistor values

