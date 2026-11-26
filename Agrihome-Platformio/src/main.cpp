#define MQTT_MAX_PACKET_SIZE 1024

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <DHT.h>
#include <BH1750.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// =====================================================
// Agrihome ESP32-S3
// MQTT + Node-RED + BLE V13 lectura/polling sin onRead
// =====================================================

#define WIFI_SSID       "Nombre del punto WIFI"
#define WIFI_PASSWORD   "Contraseña del punto WIFI"

#define MQTT_HOST       "IP en cuestion"
#define MQTT_PORT       1883
#define MQTT_CLIENT_ID  "agrihome-esp32s3-01"

#define TOPIC_TELEMETRY "agrihome/telemetry"
#define TOPIC_STATUS    "agrihome/status"
#define TOPIC_COMMAND   "agrihome/cmd"

#define BLE_DEVICE_NAME "Agrihome_BLE"
#define BLE_SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define BLE_TELEMETRY_UUID      "12345678-1234-1234-1234-1234567890ac"  // sensores
#define BLE_COMMAND_UUID        "12345678-1234-1234-1234-1234567890ad"  // comandos
#define BLE_STATE_UUID          "12345678-1234-1234-1234-1234567890ae"  // estados
#define BLE_CONFIG_UUID         "12345678-1234-1234-1234-1234567890af"  // config/hora

#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   9
#define PIN_SOIL1_ADC 4
#define PIN_UV_ADC    5
#define PIN_SOIL2_ADC 6
#define PIN_DHT       7
#define DHTTYPE       DHT11
#define PIN_PUMP      12
#define PIN_LEDS      15

#define PUMP_ACTIVE_LOW false
#define LEDS_ACTIVE_LOW false

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
DHT dht(PIN_DHT, DHTTYPE);
BH1750 bh1750;
Preferences preferences;

BLEServer *bleServer = nullptr;
BLECharacteristic *bleTelemetryChar = nullptr;
BLECharacteristic *bleCommandChar = nullptr;
BLECharacteristic *bleStateChar = nullptr;
BLECharacteristic *bleConfigChar = nullptr;
bool bleDeviceConnected = false;

// Cola de comandos BLE.
// El callback GATT solo guarda el comando; loop() lo procesa después.
volatile bool hasPendingBleCommand = false;
String pendingBleCommand = "";

const unsigned long TELEMETRY_INTERVAL_MS = 1000;
unsigned long lastBleValueUpdate = 0;
const unsigned long BLE_VALUE_UPDATE_MS = 1000;
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const uint8_t ADC_SAMPLES = 15;

unsigned long lastTelemetry = 0;
unsigned long lastMqttReconnectAttempt = 0;

bool pump_state = false;
bool leds_state = false;
bool modo_autonomo = false;

int last_soil1_raw = 0;
int last_soil2_raw = 0;
int last_soil_avg_raw = 0;
int last_uv_raw = 0;
float last_lux = 0;
float last_temperature_c = 0;
float last_humidity_pct = 0;

int soil_dry_threshold = 1500;
float lux_low_threshold = 100.0;
unsigned long pump_on_time_ms = 3000;
unsigned long pump_cooldown_ms = 15000;

int current_hour = 12;
int current_minute = 0;
unsigned long lastClockTickMs = 0;

bool night_mode_enabled = true;
int night_start_hour = 20;
int night_end_hour = 6;

unsigned long pumpStartedAt = 0;
unsigned long lastPumpCycleAt = 0;
bool autoPumpRunning = false;

bool soil1_dry_state = false;
bool soil2_dry_state = false;
bool any_soil_dry_state = false;
bool low_light_state = false;
bool night_time_state = false;
bool leds_blocked_by_night = false;

// Prototipos
void updateBleValues();
void readSensors();
void handleAutonomousControl();
void publishTelemetry();
void publishStatus(const char *msg);
void buildStateJson(JsonDocument &doc);
void handleCommandString(String input, const char *source);
void applySetConfig(JsonDocument &doc);
void saveConfig();
void loadConfig();
void setPump(bool state);
void setLeds(bool state);
bool isNightTime();
void connectWiFi();
void connectMQTT();
void setupBLE();
void mqttCallback(char *topic, byte *payload, unsigned int length);

void updateManualClock() {
  unsigned long now = millis();
  if (lastClockTickMs == 0) {
    lastClockTickMs = now;
    return;
  }

  while (now - lastClockTickMs >= 60000UL) {
    lastClockTickMs += 60000UL;
    current_minute++;
    if (current_minute >= 60) {
      current_minute = 0;
      current_hour++;
      if (current_hour >= 24) current_hour = 0;
    }
  }
}

bool isNightTime() {
  int h = constrain(current_hour, 0, 23);
  int start = constrain(night_start_hour, 0, 23);
  int end = constrain(night_end_hour, 0, 23);
  if (start == end) return false;
  if (start < end) return h >= start && h < end;
  return h >= start || h < end;
}

class AgrihomeServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    bleDeviceConnected = true;
    Serial.println("BLE conectado");
    updateBleValues();
  }

  void onDisconnect(BLEServer *server) override {
    bleDeviceConnected = false;
    Serial.println("BLE desconectado");
    delay(500);
    BLEDevice::startAdvertising();
    Serial.println("BLE advertising reiniciado");
  }
};

class AgrihomeCommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    std::string value = characteristic->getValue();
    if (value.length() == 0) return;

    String cmd;
    for (size_t i = 0; i < value.length(); i++) cmd += (char)value[i];
    cmd.trim();

    // No ejecutar lógica pesada dentro del callback GATT.
    // Solo guardar el comando y procesarlo en loop().
    pendingBleCommand = cmd;
    hasPendingBleCommand = true;

    Serial.print("Comando BLE en cola: ");
    Serial.println(cmd);
  }
};

// Nota V13: sin callbacks onRead. Las características BLE se actualizan desde loop/publishTelemetry


void loadConfig() {
  preferences.begin("agrihome", false);
  modo_autonomo = preferences.getBool("auto", false);
  soil_dry_threshold = preferences.getInt("soil_thr", 1500);
  lux_low_threshold = preferences.getFloat("lux_thr", 100.0);
  pump_on_time_ms = preferences.getULong("pump_on", 3000);
  pump_cooldown_ms = preferences.getULong("pump_cd", 15000);
  current_hour = preferences.getInt("hour", 12);
  current_minute = preferences.getInt("minute", 0);
  night_mode_enabled = preferences.getBool("night", true);
  night_start_hour = preferences.getInt("n_start", 20);
  night_end_hour = preferences.getInt("n_end", 6);
  preferences.end();

  current_hour = constrain(current_hour, 0, 23);
  current_minute = constrain(current_minute, 0, 59);
  night_start_hour = constrain(night_start_hour, 0, 23);
  night_end_hour = constrain(night_end_hour, 0, 23);

  Serial.println("Configuración cargada:");
  Serial.print("modo_autonomo="); Serial.println(modo_autonomo ? "true" : "false");
  Serial.print("soil_dry_threshold="); Serial.println(soil_dry_threshold);
  Serial.print("lux_low_threshold="); Serial.println(lux_low_threshold);
  Serial.print("pump_on_time_ms="); Serial.println(pump_on_time_ms);
  Serial.print("pump_cooldown_ms="); Serial.println(pump_cooldown_ms);
  Serial.print("hora="); Serial.print(current_hour); Serial.print(":"); Serial.println(current_minute);
  Serial.print("night_mode_enabled="); Serial.println(night_mode_enabled ? "true" : "false");
  Serial.print("night_start_hour="); Serial.println(night_start_hour);
  Serial.print("night_end_hour="); Serial.println(night_end_hour);
}

void saveConfig() {
  preferences.begin("agrihome", false);
  preferences.putBool("auto", modo_autonomo);
  preferences.putInt("soil_thr", soil_dry_threshold);
  preferences.putFloat("lux_thr", lux_low_threshold);
  preferences.putULong("pump_on", pump_on_time_ms);
  preferences.putULong("pump_cd", pump_cooldown_ms);
  preferences.putInt("hour", current_hour);
  preferences.putInt("minute", current_minute);
  preferences.putBool("night", night_mode_enabled);
  preferences.putInt("n_start", night_start_hour);
  preferences.putInt("n_end", night_end_hour);
  preferences.end();
}

int readAdcAverage(uint8_t pin) {
  long acc = 0;
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) {
    acc += analogRead(pin);
    delay(2);
  }
  return acc / ADC_SAMPLES;
}

void setPump(bool state) {
  pump_state = state;
  digitalWrite(PIN_PUMP, PUMP_ACTIVE_LOW ? !state : state);
  Serial.print("BOMBA: "); Serial.println(state ? "ON" : "OFF");
}

void setLeds(bool state) {
  leds_state = state;
  digitalWrite(PIN_LEDS, LEDS_ACTIVE_LOW ? !state : state);
  Serial.print("LEDS UV: "); Serial.println(state ? "ON" : "OFF");
}

void readSensors() {
  last_soil1_raw = readAdcAverage(PIN_SOIL1_ADC);
  last_soil2_raw = readAdcAverage(PIN_SOIL2_ADC);
  last_soil_avg_raw = (last_soil1_raw + last_soil2_raw) / 2;
  last_uv_raw = readAdcAverage(PIN_UV_ADC);

  float luxValue = bh1750.readLightLevel();
  if (!isnan(luxValue) && luxValue >= 0) last_lux = luxValue;

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) last_temperature_c = t;
  if (!isnan(h)) last_humidity_pct = h;
}

void handleAutonomousControl() {
  soil1_dry_state = last_soil1_raw > soil_dry_threshold;
  soil2_dry_state = last_soil2_raw > soil_dry_threshold;
  any_soil_dry_state = soil1_dry_state || soil2_dry_state;

  low_light_state = last_lux < lux_low_threshold;
  night_time_state = isNightTime();
  leds_blocked_by_night = night_mode_enabled && night_time_state;

  unsigned long now = millis();

  if (!modo_autonomo) {
    autoPumpRunning = false;
    return;
  }

  if (low_light_state && !leds_blocked_by_night) setLeds(true);
  else setLeds(false);

  if (autoPumpRunning) {
    if (now - pumpStartedAt >= pump_on_time_ms) {
      setPump(false);
      autoPumpRunning = false;
      lastPumpCycleAt = now;
    }
    return;
  }

  bool cooldownOk = (lastPumpCycleAt == 0) || (now - lastPumpCycleAt >= pump_cooldown_ms);
  if (any_soil_dry_state && cooldownOk) {
    setPump(true);
    autoPumpRunning = true;
    pumpStartedAt = now;
  }
}

void buildStateJson(JsonDocument &doc) {
  doc["project"] = "Agrihome";
  doc["soil1_raw"] = last_soil1_raw;
  doc["soil2_raw"] = last_soil2_raw;
  doc["soil_avg_raw"] = last_soil_avg_raw;
  doc["lux"] = last_lux;
  doc["uv_raw"] = last_uv_raw;
  doc["temperature_c"] = last_temperature_c;
  doc["humidity_pct"] = last_humidity_pct;
  doc["pump_state"] = pump_state;
  doc["leds_state"] = leds_state;
  doc["modo_autonomo"] = modo_autonomo;
  doc["soil1_dry"] = soil1_dry_state;
  doc["soil2_dry"] = soil2_dry_state;
  doc["any_soil_dry"] = any_soil_dry_state;
  doc["low_light"] = low_light_state;
  doc["auto_pump_running"] = autoPumpRunning;
  doc["soil_dry_threshold"] = soil_dry_threshold;
  doc["lux_low_threshold"] = lux_low_threshold;
  doc["pump_on_time_ms"] = pump_on_time_ms;
  doc["pump_cooldown_ms"] = pump_cooldown_ms;
  doc["current_hour"] = current_hour;
  doc["current_minute"] = current_minute;
  doc["night_mode_enabled"] = night_mode_enabled;
  doc["night_start_hour"] = night_start_hour;
  doc["night_end_hour"] = night_end_hour;
  doc["night_time"] = night_time_state;
  doc["is_night_time"] = night_time_state;
  doc["leds_blocked_by_night"] = leds_blocked_by_night;
  doc["wifi_rssi"] = WiFi.RSSI();
  doc["ble_connected"] = bleDeviceConnected;
  doc["mqtt_connected"] = mqtt.connected();
  doc["uptime_ms"] = millis();
}

void updateBleValues() {
  if (bleTelemetryChar == nullptr || bleStateChar == nullptr || bleConfigChar == nullptr) return;

  StaticJsonDocument<192> p1;
  p1["p"] = 1;
  p1["s1"] = last_soil1_raw;
  p1["s2"] = last_soil2_raw;
  p1["sa"] = last_soil_avg_raw;
  p1["lux"] = last_lux;
  p1["uv"] = last_uv_raw;
  p1["t"] = last_temperature_c;
  p1["h"] = last_humidity_pct;
  p1["rssi"] = WiFi.RSSI();
  p1["up"] = millis();
  char out1[192];
  size_t n1 = serializeJson(p1, out1, sizeof(out1));
  bleTelemetryChar->setValue((uint8_t *)out1, n1);

  StaticJsonDocument<192> p2;
  p2["p"] = 2;
  p2["pump"] = pump_state ? 1 : 0;
  p2["leds"] = leds_state ? 1 : 0;
  p2["auto"] = modo_autonomo ? 1 : 0;
  p2["d1"] = soil1_dry_state ? 1 : 0;
  p2["d2"] = soil2_dry_state ? 1 : 0;
  p2["dry"] = any_soil_dry_state ? 1 : 0;
  p2["low"] = low_light_state ? 1 : 0;
  p2["ap"] = autoPumpRunning ? 1 : 0;
  p2["soil_thr"] = soil_dry_threshold;
  p2["lux_thr"] = lux_low_threshold;
  char out2[192];
  size_t n2 = serializeJson(p2, out2, sizeof(out2));
  bleStateChar->setValue((uint8_t *)out2, n2);

  StaticJsonDocument<192> p3;
  p3["p"] = 3;
  p3["pon"] = pump_on_time_ms;
  p3["pcd"] = pump_cooldown_ms;
  p3["hour"] = current_hour;
  p3["minute"] = current_minute;
  p3["night"] = night_mode_enabled ? 1 : 0;
  p3["night_start"] = night_start_hour;
  p3["night_end"] = night_end_hour;
  p3["night_time"] = night_time_state ? 1 : 0;
  p3["block_leds"] = leds_blocked_by_night ? 1 : 0;
  char out3[192];
  size_t n3 = serializeJson(p3, out3, sizeof(out3));
  bleConfigChar->setValue((uint8_t *)out3, n3);
}

void publishTelemetry() {
  readSensors();
  handleAutonomousControl();
  updateBleValues();

  StaticJsonDocument<1024> doc;
  buildStateJson(doc);

  char out[1024];
  size_t n = serializeJson(doc, out, sizeof(out));

  bool ok = false;
  if (mqtt.connected()) ok = mqtt.publish(TOPIC_TELEMETRY, out, n);

  Serial.println();
  Serial.println("TELEMETRIA:");
  Serial.println(out);
  Serial.print("telemetry_size="); Serial.println(n);
  Serial.print("mqtt_publish="); Serial.println(ok ? "OK" : "SKIP/FAIL");
  Serial.print("AUTO="); Serial.print(modo_autonomo ? "ON" : "OFF");
  Serial.print(" | soil1="); Serial.print(last_soil1_raw);
  Serial.print(" | soil2="); Serial.print(last_soil2_raw);
  Serial.print(" | soil_thr="); Serial.print(soil_dry_threshold);
  Serial.print(" | lux="); Serial.print(last_lux);
  Serial.print(" | lux_thr="); Serial.print(lux_low_threshold);
  Serial.print(" | hora="); Serial.print(current_hour); Serial.print(":"); Serial.print(current_minute);
  Serial.print(" | night="); Serial.print(night_time_state ? "SI" : "NO");
  Serial.print(" | block_leds="); Serial.print(leds_blocked_by_night ? "SI" : "NO");
  Serial.print(" | bomba="); Serial.print(pump_state ? "ON" : "OFF");
  Serial.print(" | leds="); Serial.println(leds_state ? "ON" : "OFF");
}

void publishStatus(const char *msg) {
  readSensors();
  handleAutonomousControl();
  updateBleValues();

  StaticJsonDocument<1024> doc;
  buildStateJson(doc);
  doc["status"] = msg;

  char out[1024];
  size_t n = serializeJson(doc, out, sizeof(out));

  if (mqtt.connected()) mqtt.publish(TOPIC_STATUS, out, n);

  Serial.print("STATUS: ");
  Serial.println(out);
}

void applySetConfig(JsonDocument &doc) {
  if (doc.containsKey("soil_dry_threshold")) soil_dry_threshold = doc["soil_dry_threshold"].as<int>();
  if (doc.containsKey("soil_thr")) soil_dry_threshold = doc["soil_thr"].as<int>();

  if (doc.containsKey("lux_low_threshold")) lux_low_threshold = doc["lux_low_threshold"].as<float>();
  if (doc.containsKey("lux_thr")) lux_low_threshold = doc["lux_thr"].as<float>();

  if (doc.containsKey("pump_on_time_ms")) pump_on_time_ms = doc["pump_on_time_ms"].as<unsigned long>();
  if (doc.containsKey("pon")) pump_on_time_ms = doc["pon"].as<unsigned long>();

  if (doc.containsKey("pump_cooldown_ms")) pump_cooldown_ms = doc["pump_cooldown_ms"].as<unsigned long>();
  if (doc.containsKey("pcd")) pump_cooldown_ms = doc["pcd"].as<unsigned long>();

  if (doc.containsKey("night_mode_enabled")) night_mode_enabled = doc["night_mode_enabled"].as<bool>();
  if (doc.containsKey("night")) night_mode_enabled = doc["night"].as<bool>();

  if (doc.containsKey("current_hour")) current_hour = constrain(doc["current_hour"].as<int>(), 0, 23);
  if (doc.containsKey("hour")) current_hour = constrain(doc["hour"].as<int>(), 0, 23);

  if (doc.containsKey("current_minute")) current_minute = constrain(doc["current_minute"].as<int>(), 0, 59);
  if (doc.containsKey("minute")) current_minute = constrain(doc["minute"].as<int>(), 0, 59);

  if (doc.containsKey("night_start_hour")) night_start_hour = constrain(doc["night_start_hour"].as<int>(), 0, 23);
  if (doc.containsKey("night_start")) night_start_hour = constrain(doc["night_start"].as<int>(), 0, 23);

  if (doc.containsKey("night_end_hour")) night_end_hour = constrain(doc["night_end_hour"].as<int>(), 0, 23);
  if (doc.containsKey("night_end")) night_end_hour = constrain(doc["night_end"].as<int>(), 0, 23);

  soil_dry_threshold = constrain(soil_dry_threshold, 0, 4095);
  lux_low_threshold = constrain(lux_low_threshold, 0.0f, 10000.0f);
  pump_on_time_ms = constrain((long)pump_on_time_ms, 100, 20000);
  pump_cooldown_ms = constrain((long)pump_cooldown_ms, 1000, 120000);

  lastClockTickMs = millis();
  saveConfig();
  readSensors();
  handleAutonomousControl();
  updateBleValues();
  publishStatus("ACK CONFIG_SAVED");
}

void handleCommandString(String input, const char *source) {
  input.trim();
  if (input.length() == 0) return;

  Serial.print("CMD["); Serial.print(source); Serial.print("]: "); Serial.println(input);

  if (input.startsWith("{")) {
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, input);
    if (err) {
      Serial.print("JSON inválido: "); Serial.println(err.c_str());
      publishStatus("ERR JSON_INVALID");
      return;
    }

    String cmd = doc["cmd"] | "";
    cmd.trim();

    if (cmd == "SET_CONFIG") {
      applySetConfig(doc);
      return;
    }

    if (cmd == "STATUS") {
      publishStatus("ACK STATUS");
      return;
    }

    publishStatus("ERR JSON_CMD_UNKNOWN");
    return;
  }

  if (input == "PUMP_ON") {
    setPump(true);
    publishStatus("ACK PUMP_ON");
  } else if (input == "PUMP_OFF") {
    setPump(false);
    autoPumpRunning = false;
    publishStatus("ACK PUMP_OFF");
  } else if (input == "LEDS_ON") {
    setLeds(true);
    publishStatus("ACK LEDS_ON");
  } else if (input == "LEDS_OFF") {
    setLeds(false);
    publishStatus("ACK LEDS_OFF");
  } else if (input == "AUTO_ON") {
    modo_autonomo = true;
    saveConfig();
    readSensors();
    handleAutonomousControl();
    publishStatus("ACK AUTO_ON");
  } else if (input == "AUTO_OFF") {
    modo_autonomo = false;
    autoPumpRunning = false;
    setPump(false);
    saveConfig();
    publishStatus("ACK AUTO_OFF");
  } else if (input == "STATUS") {
    publishStatus("ACK STATUS");
  } else {
    publishStatus("ERR CMD_UNKNOWN");
  }
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String input;
  for (unsigned int i = 0; i < length; i++) input += (char)payload[i];
  handleCommandString(input, "MQTT");
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Conectando WiFi");
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK. IP ESP32: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi no conectado. BLE sigue disponible.");
  }
}

void connectMQTT() {
  if (mqtt.connected()) return;
  Serial.print("Conectando MQTT...");
  if (mqtt.connect(MQTT_CLIENT_ID)) {
    Serial.println("OK");
    mqtt.subscribe(TOPIC_COMMAND);
    publishStatus("MQTT_CONNECTED");
  } else {
    Serial.print("fallo rc="); Serial.println(mqtt.state());
  }
}

void setupBLE() {
  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(185);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new AgrihomeServerCallbacks());

  BLEService *service = bleServer->createService(BLE_SERVICE_UUID);

  bleTelemetryChar = service->createCharacteristic(
    BLE_TELEMETRY_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  bleStateChar = service->createCharacteristic(
    BLE_STATE_UUID,
    BLECharacteristic::PROPERTY_READ
  );
  bleConfigChar = service->createCharacteristic(
    BLE_CONFIG_UUID,
    BLECharacteristic::PROPERTY_READ
  );

  bleCommandChar = service->createCharacteristic(
    BLE_COMMAND_UUID,
    BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_WRITE
  );
  bleCommandChar->setCallbacks(new AgrihomeCommandCallbacks());

  updateBleValues();
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  BLEAdvertisementData advertisementData;
  advertisementData.setName(BLE_DEVICE_NAME);
  advertisementData.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));

  BLEAdvertisementData scanResponseData;
  scanResponseData.setName(BLE_DEVICE_NAME);

  advertising->setAdvertisementData(advertisementData);
  advertising->setScanResponseData(scanResponseData);
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  advertising->setMinPreferred(0x06);
  advertising->setMinPreferred(0x12);

  BLEDevice::startAdvertising();
  Serial.println("BLE iniciado como Agrihome_BLE");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("======================================");
  Serial.println("Agrihome ESP32-S3 MQTT + BLE V13 SIN ONREAD + Modo Noche");
  Serial.println("======================================");

  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_LEDS, OUTPUT);
  setPump(false);
  setLeds(false);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  loadConfig();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (bh1750.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("BH1750 detectado correctamente");
  } else {
    Serial.println("BH1750 no detectado");
  }
  dht.begin();

  readSensors();
  handleAutonomousControl();

  setupBLE();

  connectWiFi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  connectMQTT();

  publishStatus("SYSTEM_READY");
  publishTelemetry();
}

void loop() {
  unsigned long now = millis();
  updateManualClock();

  // Procesar comandos BLE fuera del callback BLE.
  // Esto mantiene libre el stack GATT y evita desconexión inmediata por timeout.
  if (hasPendingBleCommand) {
    String cmd = pendingBleCommand;
    pendingBleCommand = "";
    hasPendingBleCommand = false;

    Serial.print("Comando BLE recibido: ");
    Serial.println(cmd);
    handleCommandString(cmd, "BLE");
    updateBleValues();
  }

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastWifiAttempt = 0;
    if (now - lastWifiAttempt >= 10000) {
      lastWifiAttempt = now;
      Serial.println("WiFi desconectado. Intentando reconectar...");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) {
      if (now - lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL_MS) {
        lastMqttReconnectAttempt = now;
        connectMQTT();
      }
    } else {
      mqtt.loop();
    }
  }

  if (now - lastBleValueUpdate >= BLE_VALUE_UPDATE_MS) {
    lastBleValueUpdate = now;
    updateBleValues();
  }

  if (now - lastTelemetry >= TELEMETRY_INTERVAL_MS) {
    lastTelemetry = now;
    publishTelemetry();
  }
}
