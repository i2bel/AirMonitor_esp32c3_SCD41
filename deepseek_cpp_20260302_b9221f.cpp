#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SensirionI2cScd4x.h>
#include <Preferences.h>
#include <Update.h>
#include <time.h>
#include <LittleFS.h>

// ================ НАСТРОЙКИ ================
#define DEFAULT_MQTT_PORT 1883
#define I2C_SDA 8
#define I2C_SCL 9
const uint8_t SCD41_ADDR = 0x62;
#define RESET_PIN 2
#define HISTORY_SIZE 288               // 24 часа * 60 / 5

// ================ ГЛОБАЛЬНЫЕ ОБЪЕКТЫ ================
SensirionI2cScd4x scd41;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
AsyncWebServer webServer(80);
Preferences preferences;

// ================ ДАННЫЕ ================
String deviceId;
String deviceName = "Air Monitor";

String wifiSSID = "", wifiPass = "";
String mqttServer = "";
int mqttPort = DEFAULT_MQTT_PORT;
String mqttUser = "", mqttPass = "";

bool configMode = false;
unsigned long lastMqttPublish = 0;
const unsigned long MQTT_INTERVAL = 60000; // 1 минута

// Исправление 1: РАСКОММЕНТИРОВАНА структура HistoryState
struct HistoryPoint {
  float value;
  time_t time;
};

struct HistoryState {
  int index;
  int count;
  HistoryPoint data[HISTORY_SIZE];
};

// Текущие показания
struct SensorData {
  uint16_t co2 = 0;
  float temperature = 0.0;
  float humidity = 0.0;
  unsigned long lastReading = 0;
} currentData;

// История с временными метками
HistoryPoint co2History[HISTORY_SIZE];
HistoryPoint tempHistory[HISTORY_SIZE];
HistoryPoint humHistory[HISTORY_SIZE];

int co2Index = 0, tempIndex = 0, humIndex = 0;
int co2Count = 0, tempCount = 0, humCount = 0;

float sumCO2 = 0, sumTemp = 0, sumHum = 0;
int measurementCount = 0;
unsigned long lastHistoryUpdate = 0;
const unsigned long HISTORY_INTERVAL = 300000; // 5 минут

// Исправление 2: ОБНОВЛЕНА функция saveHistoryToFS с правильным копированием
void saveHistoryToFS() {
  // CO2
  HistoryState co2State;
  co2State.index = co2Index;
  co2State.count = co2Count;
  memcpy(co2State.data, co2History, sizeof(co2History));
  
  File file = LittleFS.open("/co2.bin", "w");
  if (file) {
    file.write((uint8_t*)&co2State, sizeof(co2State));
    file.close();
  }

  // Температура
  HistoryState tempState;
  tempState.index = tempIndex;
  tempState.count = tempCount;
  memcpy(tempState.data, tempHistory, sizeof(tempHistory));
  
  file = LittleFS.open("/temp.bin", "w");
  if (file) {
    file.write((uint8_t*)&tempState, sizeof(tempState));
    file.close();
  }

  // Влажность
  HistoryState humState;
  humState.index = humIndex;
  humState.count = humCount;
  memcpy(humState.data, humHistory, sizeof(humHistory));
  
  file = LittleFS.open("/hum.bin", "w");
  if (file) {
    file.write((uint8_t*)&humState, sizeof(humState));
    file.close();
  }

  Serial.println("History saved to LittleFS");
}

// Исправление 2: ОБНОВЛЕНА функция loadHistoryFromFS с нормализацией времени
void loadHistoryFromFS() {
  time_t now = time(nullptr);
  time_t cutoffTime = now - (24 * 3600); // 24 часа назад
  
  File file;

  // CO2
  file = LittleFS.open("/co2.bin", "r");
  if (file) {
    size_t fileSize = file.size();
    Serial.printf("Found CO2 history file, size: %u bytes\n", fileSize);
    
    if (fileSize == sizeof(HistoryState)) {
      HistoryState co2State;
      if (file.read((uint8_t*)&co2State, sizeof(co2State)) == sizeof(co2State)) {
        
        // Нормализация времени - оставляем только точки за последние 24 часа
        int newCount = 0;
        int newIndex = 0;
        HistoryPoint tempBuffer[HISTORY_SIZE];
        
        for (int i = 0; i < co2State.count; i++) {
          int idx = (co2State.index - co2State.count + i + HISTORY_SIZE) % HISTORY_SIZE;
          
          // Если точка не старше 24 часов
          if (co2State.data[idx].time >= cutoffTime) {
            tempBuffer[newIndex] = co2State.data[idx];
            newIndex = (newIndex + 1) % HISTORY_SIZE;
            newCount++;
          }
        }
        
        // Копируем отфильтрованные данные
        memcpy(co2History, tempBuffer, sizeof(tempBuffer));
        co2Index = newIndex;
        co2Count = newCount;
        
        Serial.printf("Loaded CO2 history: %d valid points of last 24h\n", newCount);
      } else {
        Serial.println("Failed to read CO2 history file");
      }
    } else {
      Serial.println("CO2 history file size mismatch, starting fresh");
    }
    file.close();
  } else {
    Serial.println("No CO2 history file found, starting fresh");
  }

  // Температура
  file = LittleFS.open("/temp.bin", "r");
  if (file) {
    size_t fileSize = file.size();
    Serial.printf("Found Temp history file, size: %u bytes\n", fileSize);
    
    if (fileSize == sizeof(HistoryState)) {
      HistoryState tempState;
      if (file.read((uint8_t*)&tempState, sizeof(tempState)) == sizeof(tempState)) {
        
        // Нормализация времени
        int newCount = 0;
        int newIndex = 0;
        HistoryPoint tempBuffer[HISTORY_SIZE];
        
        for (int i = 0; i < tempState.count; i++) {
          int idx = (tempState.index - tempState.count + i + HISTORY_SIZE) % HISTORY_SIZE;
          
          if (tempState.data[idx].time >= cutoffTime) {
            tempBuffer[newIndex] = tempState.data[idx];
            newIndex = (newIndex + 1) % HISTORY_SIZE;
            newCount++;
          }
        }
        
        memcpy(tempHistory, tempBuffer, sizeof(tempBuffer));
        tempIndex = newIndex;
        tempCount = newCount;
        
        Serial.printf("Loaded Temp history: %d valid points of last 24h\n", newCount);
      } else {
        Serial.println("Failed to read Temp history file");
      }
    } else {
      Serial.println("Temp history file size mismatch, starting fresh");
    }
    file.close();
  } else {
    Serial.println("No Temp history file found, starting fresh");
  }

  // Влажность
  file = LittleFS.open("/hum.bin", "r");
  if (file) {
    size_t fileSize = file.size();
    Serial.printf("Found Hum history file, size: %u bytes\n", fileSize);
    
    if (fileSize == sizeof(HistoryState)) {
      HistoryState humState;
      if (file.read((uint8_t*)&humState, sizeof(humState)) == sizeof(humState)) {
        
        // Нормализация времени
        int newCount = 0;
        int newIndex = 0;
        HistoryPoint tempBuffer[HISTORY_SIZE];
        
        for (int i = 0; i < humState.count; i++) {
          int idx = (humState.index - humState.count + i + HISTORY_SIZE) % HISTORY_SIZE;
          
          if (humState.data[idx].time >= cutoffTime) {
            tempBuffer[newIndex] = humState.data[idx];
            newIndex = (newIndex + 1) % HISTORY_SIZE;
            newCount++;
          }
        }
        
        memcpy(humHistory, tempBuffer, sizeof(tempBuffer));
        humIndex = newIndex;
        humCount = newCount;
        
        Serial.printf("Loaded Hum history: %d valid points of last 24h\n", newCount);
      } else {
        Serial.println("Failed to read Hum history file");
      }
    } else {
      Serial.println("Hum history file size mismatch, starting fresh");
    }
    file.close();
  } else {
    Serial.println("No Hum history file found, starting fresh");
  }
}

// ================ ПРОТОТИПЫ ================
bool initSensor();
bool readSensor();
void addToHistory(HistoryPoint *history, int &idx, int &cnt, float value, time_t t);
void loadConfig();
void saveConfig(String wifiSsid, String wifiPass,
                String mqttServer, int mqttPort,
                String mqttUser, String mqttPass);
void clearConfig();
void startConfigMode();
void connectToWiFi();
void reconnectMQTT();
void publishDiscoveryConfigs();
void publishMQTTData();
String getHTMLPage();
void setupWebServer();
void handleOTAUpdate(AsyncWebServerRequest *request);

// ================ ИНИЦИАЛИЗАЦИЯ ДАТЧИКА ================
bool initSensor() {
  Wire.begin(I2C_SDA, I2C_SCL);
  scd41.begin(Wire, SCD41_ADDR);

  // Критически важная строка из рабочей версии!
  scd41.stopPeriodicMeasurement();
  delay(500);

  uint64_t serialNumber = 0;
  int16_t error = scd41.getSerialNumber(serialNumber);
  if (error != 0) {
    Serial.print("Sensor init error: 0x");
    Serial.println(error, HEX);
    return false;
  }
  Serial.print("Sensor found! SN: 0x");
  Serial.println(serialNumber, HEX);
  return true;
}

bool readSensor() {
  static unsigned long lastMeasureTime = 0;
  static bool measurementStarted = false;
  static unsigned long measurementStartTime = 0;

  // Начинаем новое измерение раз в 30 секунд (или как у вас)
  if (!measurementStarted && millis() - lastMeasureTime >= 30000) {
    int16_t error = scd41.measureSingleShot();
    if (error != 0) {
      Serial.print("measureSingleShot error: 0x");
      Serial.println(error, HEX);
      return false;
    }
    measurementStarted = true;
    measurementStartTime = millis();
    return false; // данные ещё не готовы
  }

  // Если измерение запущено, ждём 5 секунд, но не блокируя
  if (measurementStarted) {
    if (millis() - measurementStartTime < 5000) {
      return false; // ещё не прошло 5 секунд
    }

    // 5 секунд прошло – читаем результат
    uint16_t co2;
    float temp, hum;
    int16_t error = scd41.readMeasurement(co2, temp, hum);
    measurementStarted = false;
    lastMeasureTime = millis();

    if (error != 0 || co2 == 0) {
      Serial.println("readMeasurement failed or co2=0");
      return false;
    }

    currentData.co2 = co2;
    currentData.temperature = temp;
    currentData.humidity = hum;

    sumCO2 += co2; sumTemp += temp; sumHum += hum; measurementCount++;
    Serial.printf("CO2: %d ppm, Temp: %.1f°C, Hum: %.1f%%\n", co2, temp, hum);
    return true;
  }

  return false;
}

// ================ ДОБАВЛЕНИЕ В ИСТОРИЮ ================
void addToHistory(HistoryPoint *history, int &idx, int &cnt, float value, time_t t) {
  history[idx].value = value;
  history[idx].time = t;
  idx = (idx + 1) % HISTORY_SIZE;
  if (cnt < HISTORY_SIZE) cnt++;
}

// ================ НАСТРОЙКИ ================
void loadConfig() {
  preferences.begin("config", false);
  wifiSSID = preferences.getString("wifi_ssid", "");
  wifiPass = preferences.getString("wifi_pass", "");
  mqttServer = preferences.getString("mqtt_server", "");
  mqttPort = preferences.getInt("mqtt_port", DEFAULT_MQTT_PORT);
  mqttUser = preferences.getString("mqtt_user", "");
  mqttPass = preferences.getString("mqtt_pass", "");
  preferences.end();
  Serial.println("Config loaded");
}

void saveConfig(String wifiSsid, String wifiPass,
                String mqttServer, int mqttPort,
                String mqttUser, String mqttPass) {
  preferences.begin("config", false);
  preferences.putString("wifi_ssid", wifiSsid);
  preferences.putString("wifi_pass", wifiPass);
  preferences.putString("mqtt_server", mqttServer);
  preferences.putInt("mqtt_port", mqttPort);
  preferences.putString("mqtt_user", mqttUser);
  preferences.putString("mqtt_pass", mqttPass);
  preferences.end();
  Serial.println("Config saved");
}

void clearConfig() {
  preferences.begin("config", false);
  preferences.clear();
  preferences.end();
  Serial.println("Config cleared");
}

// ================ AP РЕЖИМ ================
void startConfigMode() {
  configMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("AP-CO2-Monitor");

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("=== AP MODE ===");
  Serial.print("AP IP: ");
  Serial.println(apIP);
  Serial.println("Connect to WiFi 'AP-CO2-Monitor' (no password)");
  Serial.println("Open http://192.168.4.1");

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String html = R"AP_PAGE(
    <!DOCTYPE html>
    <html>
    <head>
        <meta charset="UTF-8">
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <title>Air Monitor Setup</title>
        <style>
            *{margin:0;padding:0;box-sizing:border-box;}
            body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;
                 background:linear-gradient(135deg,#f5f7fa 0%,#e9ecf2 100%);
                 min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
            .card{background:rgba(255,255,255,0.9);backdrop-filter:blur(10px);border-radius:28px;
                  padding:28px;box-shadow:0 10px 30px -10px rgba(0,0,0,0.1);max-width:500px;width:100%;}
            h1{font-size:26px;font-weight:500;color:#1d1d1f;margin-bottom:5px;text-align:center;}
            .subtitle{color:#6c6c70;font-size:14px;text-align:center;margin-bottom:24px;}
            .section{background:#ffffff;border-radius:20px;padding:20px;margin-bottom:20px;
                      box-shadow:0 2px 12px rgba(0,0,0,0.04);}
            .section h2{font-size:18px;font-weight:500;margin-bottom:16px;color:#1d1d1f;}
            label{display:block;margin:10px 0 5px;color:#6c6c70;font-size:14px;font-weight:500;}
            input{width:100%;padding:12px;border:1px solid #d9d9dc;border-radius:14px;
                   font-size:16px;background:#f9f9fb;transition:0.2s;}
            input:focus{border-color:#007aff;outline:none;background:#ffffff;}
            button{background:#007aff;color:white;border:none;padding:14px;border-radius:40px;
                   font-size:16px;font-weight:500;width:100%;cursor:pointer;transition:background 0.2s;}
            button:hover{background:#005bbf;}
            .info{text-align:center;margin-top:20px;color:#8e8e93;font-size:12px;}
        </style>
    </head>
    <body>
        <div class="card">
            <h1>Air Monitor</h1>
            <div class="subtitle">Device ID: )AP_PAGE" + deviceId + R"AP_PAGE(</div>
            <form action="/save" method="POST">
                <div class="section">
                    <h2>WiFi Settings</h2>
                    <label>SSID</label>
                    <input type="text" name="wifi_ssid" required placeholder="HomeWiFi">
                    <label>Password</label>
                    <input type="password" name="wifi_pass" required>
                </div>
                <div class="section">
                    <h2>MQTT Settings</h2>
                    <label>Server</label>
                    <input type="text" name="mqtt_server" required placeholder="192.168.1.100">
                    <label>Port</label>
                    <input type="number" name="mqtt_port" value="1883" required>
                    <label>Username (optional)</label>
                    <input type="text" name="mqtt_user">
                    <label>Password (optional)</label>
                    <input type="password" name="mqtt_pass">
                </div>
                <button type="submit">Save & Connect</button>
            </form>
            <div class="info">i2bel · github.com/i2bel</div>
        </div>
    </body>
    </html>
    )AP_PAGE";
    request->send(200, "text/html", html);
  });

  webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("wifi_ssid", true) && request->hasParam("wifi_pass", true) &&
        request->hasParam("mqtt_server", true) && request->hasParam("mqtt_port", true)) {
      String wifiSsid = request->getParam("wifi_ssid", true)->value();
      String wifiPass = request->getParam("wifi_pass", true)->value();
      String mqttServer = request->getParam("mqtt_server", true)->value();
      int mqttPort = request->getParam("mqtt_port", true)->value().toInt();
      String mqttUser = request->hasParam("mqtt_user", true) ? request->getParam("mqtt_user", true)->value() : "";
      String mqttPass = request->hasParam("mqtt_pass", true) ? request->getParam("mqtt_pass", true)->value() : "";

      saveConfig(wifiSsid, wifiPass, mqttServer, mqttPort, mqttUser, mqttPass);

      String html = R"AP_PAGE(
      <!DOCTYPE html>
      <html>
      <head><meta charset="UTF-8"><meta http-equiv="refresh" content="5;url=/"></head>
      <body style="font-family:-apple-system;background:linear-gradient(135deg,#f5f7fa 0%,#e9ecf2 100%);display:flex;align-items:center;justify-content:center;min-height:100vh;">
        <div style="background:white;border-radius:28px;padding:28px;text-align:center;max-width:400px;">
          <h2 style="font-weight:500;">✅ Settings Saved!</h2>
          <p style="color:#6c6c70;">Restarting...</p>
        </div>
      </body>
      </html>
      )AP_PAGE";
      request->send(200, "text/html", html);
      delay(1000);
      ESP.restart();
    } else {
      request->send(400, "text/plain", "Missing fields");
    }
  });

  webServer.begin();

  while (configMode) {
    if (digitalRead(RESET_PIN) == LOW) {
      delay(200);
      if (digitalRead(RESET_PIN) == LOW) {
        Serial.println("Reset in AP mode – clearing config and restarting.");
        clearConfig();
        delay(1000);
        ESP.restart();
      }
    }
    delay(100);
  }
}

// ================ ПОДКЛЮЧЕНИЕ К WIFI ================
void connectToWiFi() {
  if (wifiSSID.length() == 0) {
    startConfigMode();
    return;
  }
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    // Синхронизация времени после подключения к WiFi
    configTime(0, 0, "192.168.1.1", "2.pool.ntp.org");
    Serial.println("Syncing time...");
    time_t now = time(nullptr);
    int syncAttempts = 0;
    while (now < 100000 && syncAttempts < 20) {
      delay(500);
      now = time(nullptr);
      syncAttempts++;
    }
    if (now > 100000) {
      Serial.println("Time synced");
    } else {
      Serial.println("Time sync failed, using millis() fallback");
    }
  } else {
    Serial.println("\nWiFi failed. Entering AP mode.");
    startConfigMode();
  }
}

// ================ MQTT ================
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("MQTT connect...");
    String clientId = "ESP32C3_" + deviceId;
    if (mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str(), nullptr, 0, false, nullptr, 120)) {
      Serial.println("connected");
      publishDiscoveryConfigs();
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" retry in 5s");
      delay(5000);
    }
  }
}

void publishDiscoveryConfigs() {
  if (!mqttClient.connected()) return;
  String baseTopic = "homeassistant/sensor/" + deviceId;

  JsonDocument deviceDoc;
  deviceDoc["identifiers"][0] = deviceId;
  deviceDoc["name"] = deviceName;
  deviceDoc["model"] = "ESP32-C3 + SCD41";
  deviceDoc["manufacturer"] = "DIY";
  deviceDoc["sw_version"] = "3.0";

  char buffer[512];

  JsonDocument co2Config;
  co2Config["name"] = deviceName + " CO2";
  co2Config["unique_id"] = deviceId + "_co2";
  co2Config["state_topic"] = "sensor/" + deviceId + "/co2";
  co2Config["unit_of_measurement"] = "ppm";
  co2Config["device_class"] = "carbon_dioxide";
  co2Config["icon"] = "mdi:molecule-co2";
  co2Config["device"] = deviceDoc;
  serializeJson(co2Config, buffer);
  mqttClient.publish((baseTopic + "/co2/config").c_str(), buffer, true);

  JsonDocument tempConfig;
  tempConfig["name"] = deviceName + " Temperature";
  tempConfig["unique_id"] = deviceId + "_temperature";
  tempConfig["state_topic"] = "sensor/" + deviceId + "/temperature";
  tempConfig["unit_of_measurement"] = "°C";
  tempConfig["device_class"] = "temperature";
  tempConfig["icon"] = "mdi:thermometer";
  tempConfig["device"] = deviceDoc;
  serializeJson(tempConfig, buffer);
  mqttClient.publish((baseTopic + "/temperature/config").c_str(), buffer, true);

  JsonDocument humConfig;
  humConfig["name"] = deviceName + " Humidity";
  humConfig["unique_id"] = deviceId + "_humidity";
  humConfig["state_topic"] = "sensor/" + deviceId + "/humidity";
  humConfig["unit_of_measurement"] = "%";
  humConfig["device_class"] = "humidity";
  humConfig["icon"] = "mdi:water-percent";
  humConfig["device"] = deviceDoc;
  serializeJson(humConfig, buffer);
  mqttClient.publish((baseTopic + "/humidity/config").c_str(), buffer, true);

  JsonDocument rssiConfig;
  rssiConfig["name"] = deviceName + " RSSI";
  rssiConfig["unique_id"] = deviceId + "_rssi";
  rssiConfig["state_topic"] = "sensor/" + deviceId + "/rssi";
  rssiConfig["unit_of_measurement"] = "dBm";
  rssiConfig["device_class"] = "signal_strength";
  rssiConfig["entity_category"] = "diagnostic";
  rssiConfig["icon"] = "mdi:wifi";
  rssiConfig["device"] = deviceDoc;
  serializeJson(rssiConfig, buffer);
  mqttClient.publish((baseTopic + "/rssi/config").c_str(), buffer, true);
}

void publishMQTTData() {
  if (!mqttClient.connected() || currentData.co2 == 0) return;
  String base = "sensor/" + deviceId;
  mqttClient.publish((base + "/co2").c_str(), String(currentData.co2).c_str());
  mqttClient.publish((base + "/temperature").c_str(), String(currentData.temperature).c_str());
  mqttClient.publish((base + "/humidity").c_str(), String(currentData.humidity).c_str());
  mqttClient.publish((base + "/rssi").c_str(), String(WiFi.RSSI()).c_str());
  Serial.println("MQTT published (incl. RSSI)");
}

// ================ ВЕБ-СЕРВЕР (ОСНОВНАЯ СТРАНИЦА) ================
String getHTMLPage() {
  String html = R"MAIN_PAGE(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Air Monitor</title>
    <link rel="icon" type="image/png" href="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAzklEQVR4AcXBsU0EQQyF0S+DPnQK7EAIpUDKIBQSMQEd+ic6oCMU0AEd0AEd0AEd0AEd0AEd0AEd7O3saCSvJD/HwD8L8j6B9wnfE7wn8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wTvCbxP8D7B+wQ/MPgHXy4qBwphncwAAAAASUVORK5CYII=">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
        }

        body {
            background: linear-gradient(145deg, #f0f4fa 0%, #d9e2ef 100%);
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            padding: 20px;
        }

        .container {
            max-width: 1000px;
            width: 100%;
            margin: 0 auto;
        }

        .card {
            background: rgba(255, 255, 255, 0.6);
            backdrop-filter: blur(20px);
            -webkit-backdrop-filter: blur(20px);
            border-radius: 40px;
            padding: 30px;
            box-shadow: 0 20px 40px rgba(0, 0, 0, 0.08), inset 0 0 0 1px rgba(255, 255, 255, 0.7);
            border: 1px solid rgba(255, 255, 255, 0.5);
            margin-bottom: 20px;
        }

        .header {
            margin-bottom: 30px;
        }

        .header h1 {
            font-size: 32px;
            font-weight: 600;
            color: #1c1c1e;
            letter-spacing: -0.5px;
            text-shadow: 0 2px 5px rgba(255,255,255,0.5);
             margin-left: 30px;
        }

        .sensors {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
            gap: 20px;
            margin-bottom: 20px;
        }

        .sensor-card {
            background: rgba(255, 255, 255, 0.4);
            backdrop-filter: blur(15px);
            -webkit-backdrop-filter: blur(15px);
            border-radius: 30px;
            padding: 22px;
            box-shadow: 0 8px 20px rgba(0, 0, 0, 0.02), inset 0 0 0 1px rgba(255, 255, 255, 0.8);
            border: 1px solid rgba(255, 255, 255, 0.4);
            transition: all 0.25s ease;
            cursor: pointer;
        }

        .sensor-card:hover {
            transform: translateY(-4px);
            background: rgba(255, 255, 255, 0.6);
            box-shadow: 0 15px 30px rgba(0, 0, 0, 0.1);
        }

        .sensor-header {
            display: flex;
            align-items: center;
            gap: 18px;
        }

        .sensor-icon {
            width: 50px;
            height: 50px;
            border-radius: 25px;
            background: rgba(255, 255, 255, 0.8);
            backdrop-filter: blur(5px);
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 26px;
            color: #1c1c1e;
            box-shadow: 0 2px 10px rgba(0,0,0,0.05), inset 0 0 0 1px rgba(255,255,255,0.9);
        }

        .co2-icon { background: rgba(0, 122, 255, 0.2); color: #007aff; }
        .temp-icon { background: rgba(255, 159, 10, 0.2); color: #ff9f0a; }
        .hum-icon { background: rgba(48, 176, 192, 0.2); color: #30b0c0; }

        .sensor-info {
            flex: 1;
        }

        .sensor-label {
            font-size: 14px;
            font-weight: 500;
            color: #6c6c70;
            letter-spacing: 0.3px;
            margin-bottom: 4px;
        }

        .sensor-value {
            font-size: 44px;
            font-weight: 300;
            color: #1c1c1e;
            line-height: 1;
            display: inline-block;
        }

        .sensor-unit {
            font-size: 16px;
            color: #8e8e93;
            margin-left: 6px;
        }

        .expand-icon {
            font-size: 20px;
            color: #8e8e93;
            transition: transform 0.3s;
            margin-left: 8px;
            opacity: 0.7;
        }

        .sensor-card.expanded .expand-icon {
            transform: rotate(180deg);
        }

        .chart-wrapper {
            max-height: 0;
            overflow: hidden;
            transition: max-height 0.5s cubic-bezier(0.4, 0, 0.2, 1);
            margin-top: 0;
        }

        .sensor-card.expanded .chart-wrapper {
            max-height: 280px;
            margin-top: 20px;
        }

        canvas {
            width: 100% !important;
            height: 200px !important;
            border-radius: 20px;
            background: rgba(255,255,255,0.5);
            backdrop-filter: blur(5px);
            padding: 10px;
        }

        .footer {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-top: 25px;
            color: #6c6c70;
            font-size: 14px;
            font-weight: 400;
            border-top: 1px solid rgba(255,255,255,0.6);
            padding-top: 20px;
        }

        .footer a {
            color: #007aff;
            text-decoration: none;
            border-bottom: 1px solid transparent;
            transition: border-color 0.2s;
        }

        .footer a:hover {
            border-bottom-color: #007aff;
        }

        .ota-link {
            background: rgba(255,255,255,0.5);
            padding: 6px 14px;
            border-radius: 40px;
            color: #007aff;
            font-weight: 500;
            backdrop-filter: blur(5px);
            border: 1px solid rgba(255,255,255,0.8);
        }

        .ota-link:hover {
            background: rgba(255,255,255,0.8);
        }

        .github-link {
            color: #3a3a3c;
        }

        .github-link:hover {
            color: #007aff;
        }

        .footer-bottom {
            text-align: center;
            margin-top: 10px;
            color: #8e8e93;
            font-size: 13px;
            display: flex;
            justify-content: center;
            gap: 15px;
            flex-wrap: wrap;
        }

        .footer-bottom span {
            background: rgba(255,255,255,0.3);
            padding: 4px 12px;
            border-radius: 30px;
            backdrop-filter: blur(5px);
        }
    </style>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns/dist/chartjs-adapter-date-fns.bundle.min.js"></script>
    <script src="https://cdn.jsdelivr.net/npm/date-fns@2.30.0/locale/ru/cdn.min.js"></script>
</head>
<body>
    <div class="container">
        <div class="card">
            <div class="header">
                <h1>Air Monitor</h1>
            </div>

            <div class="sensor-card" id="card-co2">
                <div class="sensor-header">
                    <div class="sensor-icon co2-icon">○</div>
                    <div class="sensor-info">
                        <div class="sensor-label">CO₂</div>
                        <div><span class="sensor-value" id="co2-value">---</span><span class="sensor-unit">ppm</span></div>
                    </div>
                    <div class="expand-icon">▼</div>
                </div>
                <div class="chart-wrapper" id="co2-chart-wrapper">
                    <canvas id="co2-chart"></canvas>
                </div>
            </div>

            <div class="sensor-card" id="card-temp">
                <div class="sensor-header">
                    <div class="sensor-icon temp-icon">◇</div>
                    <div class="sensor-info">
                        <div class="sensor-label">Temperature</div>
                        <div><span class="sensor-value" id="temp-value">---</span><span class="sensor-unit">°C</span></div>
                    </div>
                    <div class="expand-icon">▼</div>
                </div>
                <div class="chart-wrapper" id="temp-chart-wrapper">
                    <canvas id="temp-chart"></canvas>
                </div>
            </div>

            <div class="sensor-card" id="card-hum">
                <div class="sensor-header">
                    <div class="sensor-icon hum-icon">□</div>
                    <div class="sensor-info">
                        <div class="sensor-label">Humidity</div>
                        <div><span class="sensor-value" id="hum-value">---</span><span class="sensor-unit">%</span></div>
                    </div>
                    <div class="expand-icon">▼</div>
                </div>
                <div class="chart-wrapper" id="hum-chart-wrapper">
                    <canvas id="hum-chart"></canvas>
                </div>
            </div>

            <div class="footer">
                <span class="ota-link"><a href="https://github.com/i2bel" target="_blank">github.com/i2bel</a></span>
                <span><a href="/update" class="ota-link">Firmware Update</a></span>
            </div>
            <div class="footer-bottom">
                <span>RSSI: <span id="rssi-value">---</span> dBm</span>
                <span>IP: <span id="ip-address-footer">)MAIN_PAGE" + WiFi.localIP().toString() + R"MAIN_PAGE(</span></span>
                <span><a href="#" onclick="refreshData();return false;">↻ Refresh</a></span>
            </div>
        </div>
    </div>

<script>
    let charts = { co2: null, temp: null, hum: null };

    document.querySelectorAll('.sensor-card').forEach(card => {
        card.addEventListener('click', () => card.classList.toggle('expanded'));
    });

    function initCharts() {
    // Базовая структура без форматера оси Y
    const baseOptions = {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        scales: {
            x: {
                type: 'time',
                time: {
                    unit: 'minute',
                    displayFormats: { minute: 'HH:mm', hour: 'HH:mm' },
                    tooltipFormat: 'HH:mm'
                },
                ticks: { maxTicksLimit: 8 }
            },
            y: {
                beginAtZero: false,
                grid: { color: 'rgba(0,0,0,0.05)' }
                // ticks.callback будет добавлен индивидуально
            }
        },
        plugins: { legend: { display: false } },
        elements: { point: { radius: 0 }, line: { borderWidth: 2, tension: 0.3 } }
    };

    // CO2 – целые числа
    let co2Options = JSON.parse(JSON.stringify(baseOptions));
    co2Options.scales.y.ticks = {
        callback: function(value) {
            return Math.round(value); // без десятичных знаков
        }
    };

    // Температура – два знака
    let tempOptions = JSON.parse(JSON.stringify(baseOptions));
    tempOptions.scales.y.ticks = {
        callback: function(value) {
            return value.toFixed(2);
        }
    };

    // Влажность – два знака
    let humOptions = JSON.parse(JSON.stringify(baseOptions));
    humOptions.scales.y.ticks = {
        callback: function(value) {
            return value.toFixed(2);
        }
    };

    charts.co2 = new Chart(document.getElementById('co2-chart'), {
        type: 'line',
        options: co2Options,
        data: { datasets: [{ data: [], borderColor: '#007aff', backgroundColor: 'rgba(0,122,255,0.1)', fill: true }] }
    });

    charts.temp = new Chart(document.getElementById('temp-chart'), {
        type: 'line',
        options: tempOptions,
        data: { datasets: [{ data: [], borderColor: '#ff9f0a', backgroundColor: 'rgba(255,159,10,0.1)', fill: true }] }
    });

    charts.hum = new Chart(document.getElementById('hum-chart'), {
        type: 'line',
        options: humOptions,
        data: { datasets: [{ data: [], borderColor: '#30b0c0', backgroundColor: 'rgba(48,176,192,0.1)', fill: true }] }
    });
     }

    function updateData() {
        fetch('/api/data')
            .then(response => response.json())
            .then(data => {
                console.log("Данные:", data);
                document.getElementById('co2-value').innerText = data.co2 || '---';
                document.getElementById('temp-value').innerText = data.temperature ? data.temperature.toFixed(1) : '---';
                document.getElementById('hum-value').innerText = data.humidity ? data.humidity.toFixed(1) : '---';
                document.getElementById('rssi-value').innerText = data.rssi || '---';

                if (data.co2History && charts.co2) {
                    charts.co2.data.datasets[0].data = data.co2History.map(p => ({ x: new Date(p.t * 1000), y: p.v }));
                    charts.co2.update();
                }
                if (data.tempHistory && charts.temp) {
                    charts.temp.data.datasets[0].data = data.tempHistory.map(p => ({ x: new Date(p.t * 1000), y: p.v }));
                    charts.temp.update();
                }
                if (data.humHistory && charts.hum) {
                    charts.hum.data.datasets[0].data = data.humHistory.map(p => ({ x: new Date(p.t * 1000), y: p.v }));
                    charts.hum.update();
                }
            })
            .catch(err => console.error('Ошибка:', err));
    }

    function refreshData() { updateData(); }

    window.onload = () => {
        initCharts();
        updateData();
        setInterval(updateData, 5000);
    };
</script>
</body>
</html>
)MAIN_PAGE";
  return html;
}

// ================ OTA ================
void handleOTAUpdate(AsyncWebServerRequest *request) {
  String html = R"OTA_PAGE(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Air Monitor - OTA Update</title>
    <style>
        *{margin:0;padding:0;box-sizing:border-box;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;}
        body{background:linear-gradient(135deg,#f5f7fa 0%,#e9ecf2 100%);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
        .card{background:white;border-radius:28px;padding:32px;box-shadow:0 10px 30px -10px rgba(0,0,0,0.1);max-width:500px;width:100%;}
        h1{font-size:24px;font-weight:500;color:#1d1d1f;margin-bottom:20px;}
        .file-input{background:#f5f7fa;border-radius:16px;padding:20px;border:1px dashed #c6c6c8;margin-bottom:24px;cursor:pointer;text-align:center;}
        .file-input:hover{background:#e9ecf2;}
        input[type="file"]{display:none;}
        .upload-btn{background:#007aff;color:white;border:none;padding:14px 28px;border-radius:40px;font-size:16px;font-weight:500;cursor:pointer;transition:background 0.2s;width:100%;}
        .upload-btn:hover{background:#005bbf;}
        .status{margin-top:20px;color:#6c6c70;text-align:center;}
        .back{display:inline-block;margin-top:16px;color:#007aff;text-decoration:none;}
    </style>
</head>
<body>
    <div class="card">
        <h1>Firmware Update</h1>
        <form method="POST" action="/update" enctype="multipart/form-data" id="ota-form">
            <div class="file-input" onclick="document.getElementById('file').click();">
                <span id="file-name">Choose firmware file (.bin)</span>
                <input type="file" name="update" id="file" accept=".bin" style="display:none;" onchange="document.getElementById('file-name').innerText = this.files[0] ? this.files[0].name : 'Choose firmware file (.bin)';">
            </div>
            <button type="submit" class="upload-btn">Upload & Update</button>
        </form>
        <div class="status" id="status"></div>
        <div style="text-align:center;"><a href="/" class="back">← Back to main page</a></div>
    </div>
    <script>
        document.getElementById('ota-form').addEventListener('submit', function(e) {
            e.preventDefault();
            let formData = new FormData(this);
            let statusDiv = document.getElementById('status');
            statusDiv.innerText = 'Uploading...';
            fetch('/update', { method: 'POST', body: formData })
                .then(response => response.text())
                .then(data => {
                    statusDiv.innerText = 'Update successful! Rebooting...';
                    setTimeout(() => window.location.href = '/', 3000);
                })
                .catch(err => {
                    statusDiv.innerText = 'Update failed: ' + err;
                });
        });
    </script>
</body>
</html>
)OTA_PAGE";
  request->send(200, "text/html", html);
}

void setupWebServer() {
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", getHTMLPage());
  });

  // ИСПРАВЛЕННЫЙ ОБРАБОТЧИК /api/data
  webServer.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["co2"] = currentData.co2;
    doc["temperature"] = currentData.temperature;
    doc["humidity"] = currentData.humidity;
    doc["rssi"] = WiFi.RSSI();

    // CO2 история
    JsonArray jsonCo2 = doc["co2History"].to<JsonArray>();
    for (int i = 0; i < co2Count; i++) {
      int idx = (co2Index - co2Count + i + HISTORY_SIZE) % HISTORY_SIZE;
      JsonObject point = jsonCo2.add<JsonObject>();
      point["t"] = co2History[idx].time;
      point["v"] = co2History[idx].value;
    }

    // Температура история
    JsonArray jsonTemp = doc["tempHistory"].to<JsonArray>();
    for (int i = 0; i < tempCount; i++) {
      int idx = (tempIndex - tempCount + i + HISTORY_SIZE) % HISTORY_SIZE;
      JsonObject point = jsonTemp.add<JsonObject>();
      point["t"] = tempHistory[idx].time;
      point["v"] = tempHistory[idx].value;
    }

    // Влажность история
    JsonArray jsonHum = doc["humHistory"].to<JsonArray>();
    for (int i = 0; i < humCount; i++) {
      int idx = (humIndex - humCount + i + HISTORY_SIZE) % HISTORY_SIZE;
      JsonObject point = jsonHum.add<JsonObject>();
      point["t"] = humHistory[idx].time;
      point["v"] = humHistory[idx].value;
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  webServer.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    handleOTAUpdate(request);
  });

  webServer.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", "<!DOCTYPE html><html><body style='font-family:-apple-system;text-align:center;padding:40px;'>Update successful! Rebooting...<script>setTimeout(()=>{window.location.href='/';},3000);</script></body></html>");
    delay(1000);
    ESP.restart();
  }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) {
      Serial.printf("OTA Update Start: %s\n", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    }
    if (len) {
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
    }
    if (final) {
      if (Update.end(true)) {
        Serial.printf("OTA Update Success: %u bytes\n", index + len);
      } else {
        Update.printError(Serial);
      }
    }
  });

  webServer.begin();
  Serial.println("Web server started");
}

// ================ SETUP ================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== Air Monitor Starting ===");

  uint64_t chipid = ESP.getEfuseMac();
  deviceId = String((uint16_t)(chipid >> 32), HEX) + String((uint32_t)chipid, HEX);
  Serial.print("Device ID: ");
  Serial.println(deviceId);

  pinMode(RESET_PIN, INPUT_PULLUP);
  loadConfig();

  if (!initSensor()) {
    Serial.println("Warning: sensor initialization failed. Will keep trying in loop.");
  }

  if (digitalRead(RESET_PIN) == LOW) {
    delay(200);
    if (digitalRead(RESET_PIN) == LOW) {
      Serial.println("Reset button pressed at startup – clearing config and entering AP mode.");
      clearConfig();
      wifiSSID = "";
    }
  }

  if (wifiSSID.length() == 0) {
    startConfigMode();
  } else {
    connectToWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      mqttClient.setServer(mqttServer.c_str(), mqttPort);
      setupWebServer();
    }
  }

if (!LittleFS.begin()) {
  Serial.println("LittleFS mount failed");
} else {
  Serial.println("LittleFS mounted");
  loadHistoryFromFS();  // загружаем сохранённые данные
}

}

// ================ LOOP ================
void loop() {
  if (configMode) {
    delay(100);
    return;
  }

  readSensor();

  // Обновление истории раз в 5 минут (усреднение)
  if (millis() - lastHistoryUpdate >= HISTORY_INTERVAL && measurementCount > 0) {
    time_t now = time(nullptr);
    float avgCO2 = sumCO2 / measurementCount;
    float avgTemp = sumTemp / measurementCount;
    float avgHum = sumHum / measurementCount;

    addToHistory(co2History, co2Index, co2Count, avgCO2, now);
    addToHistory(tempHistory, tempIndex, tempCount, avgTemp, now);
    addToHistory(humHistory, humIndex, humCount, avgHum, now);

    sumCO2 = 0; sumTemp = 0; sumHum = 0; measurementCount = 0;
    lastHistoryUpdate = millis();
  }

 
// MQTT
if (WiFi.status() == WL_CONNECTED && mqttServer.length() > 0) {
  if (!mqttClient.connected()) {
    Serial.print("MQTT connection lost, reason: ");
    Serial.println(mqttClient.state());
    reconnectMQTT();
  } else {
    mqttClient.loop();
  }
  if (millis() - lastMqttPublish >= MQTT_INTERVAL) {
    publishMQTTData();
    lastMqttPublish = millis();
  }
}

  // Проверка кнопки сброса
  static unsigned long pressStart = 0;
  if (digitalRead(RESET_PIN) == LOW) {
    if (pressStart == 0) pressStart = millis();
    if (millis() - pressStart > 3000) {
      Serial.println("Reset button held for 3 seconds – clearing config and restarting.");
      clearConfig();
      delay(1000);
      ESP.restart();
    }
  } else {
    pressStart = 0;
  }

  // Сохранение истории в LittleFS раз в час
  static unsigned long lastFSSave = 0;
  if (millis() - lastFSSave >= 3600000) {
    saveHistoryToFS();
    lastFSSave = millis();
  }

  delay(1000);
}