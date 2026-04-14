#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "DHT.h"

// --- 1. WIFI CONFIGURATION ---
const char* ssid = "E320-4d7";
const char* password = "AILL1113";

const int WIFI_TIMEOUT_SEC = 15;  // Give up after 15 seconds

// --- 2. SECURE HIVEMQ CREDENTIALS (PUBLISH ONLY) ---
const char* mqtt_server = "5891ec2abd8c46ccae0bf7e388a62da7.s1.eu.hivemq.cloud";
const char* mqtt_user = "node_m13";
const char* mqtt_pass = "Scotty1113!";

// --- 3. OLED CONFIGURATION ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
#define OLED_PERIOD_MS 500

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- 4. HARDWARE PINS ---
#define DHTPIN 18
#define DHTTYPE DHT11

const int pinPo = 33;            // Analog pH reading
const int pinDo = 19;            // Digital pH limit trigger
const int pinTDS = 32;           // Analog TDS reading
const int pinLightDigital = 5;   // Digital light sensor (DO pin)

// --- 5. TDS CALIBRATION ---
#define VREF 3.3
#define TDS_FACTOR 0.5           // Adjust this during calibration

// --- 6. TIMING ---
unsigned long lastOledUpdate = 0;
unsigned long lastPublish = 0;
unsigned long lastWifiRetry = 0;
const unsigned long PUBLISH_INTERVAL = 5000;
const unsigned long WIFI_RETRY_INTERVAL = 60000;  // Retry WiFi every 60 sec

// --- 7. SENSOR OBJECTS ---
DHT dht(DHTPIN, DHTTYPE);
WiFiClientSecure espClient;
PubSubClient client(espClient);

// --- 8. GLOBAL SENSOR VALUES ---
float temperature = 0;
float humidity = 0;
float phVoltage = 0;
float tdsValue = 0;
bool lightDetected = false;
int phTrigger = 0;

// --- 9. CONNECTION STATE ---
bool wifiConnected = false;
bool oledAvailable = false;

// --- WIFI CONNECTION WITH TIMEOUT ---
bool connectWiFi() {
  Serial.print("Connecting to Wi-Fi");
  if (oledAvailable) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(F("HydroLab v2.0"));
    display.println();
    display.println(F("Connecting WiFi..."));
    display.display();
  }
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < WIFI_TIMEOUT_SEC * 2) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi Connected!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    
    if (oledAvailable) {
      display.println(F("WiFi Connected!"));
      display.print(F("IP: "));
      display.println(WiFi.localIP());
      display.display();
    }
    return true;
  } else {
    Serial.println("\nWi-Fi FAILED - Running offline");
    WiFi.disconnect();
    
    if (oledAvailable) {
      display.println(F("WiFi FAILED"));
      display.println(F("Running offline..."));
      display.display();
    }
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n=== HydroLab v2.0 ===");
  
  // Initialize I2C for OLED (SDA=21, SCL=22)
  Wire.begin(21, 22);
  
  // Initialize OLED
  if(display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    oledAvailable = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("HydroLab v2.0"));
    display.println(F("Initializing..."));
    display.display();
    Serial.println("OLED: OK");
  } else {
    Serial.println("OLED: FAILED");
  }
  
  // Initialize sensors
  dht.begin();
  pinMode(pinDo, INPUT);
  pinMode(pinLightDigital, INPUT);
  Serial.println("Sensors: OK");
  
  // Try to connect to WiFi (with timeout)
  wifiConnected = connectWiFi();
  
  // Only setup MQTT if WiFi connected
  if (wifiConnected) {
    espClient.setInsecure();
    client.setServer(mqtt_server, 8883);
  }
  
  delay(1500);
}

void reconnectMQTT() {
  if (!wifiConnected) return;
  
  if (!client.connected()) {
    Serial.print("Connecting to MQTT...");
    String clientId = "ESP32_HydroLab_" + String(random(0xffff), HEX);
    
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println("OK");
    } else {
      Serial.print("Failed: ");
      Serial.println(client.state());
    }
  }
}

float readTDS(float waterTempC) {
  int analogValue = analogRead(pinTDS);
  float voltage = analogValue * (VREF / 4095.0);
  
  // Temperature compensation
  float compensationCoefficient = 1.0 + 0.02 * (waterTempC - 25.0);
  float compensatedVoltage = voltage / compensationCoefficient;
  
  // Convert voltage to TDS (ppm)
  float tds = (133.42 * pow(compensatedVoltage, 3) 
             - 255.86 * pow(compensatedVoltage, 2) 
             + 857.39 * compensatedVoltage) * TDS_FACTOR;
  
  return tds;
}

void updateOLED() {
  if (!oledAvailable) return;
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  
  // Row 1: Temperature & Humidity
  display.print(F("Temp: "));
  display.print(temperature, 1);
  display.print(F("F H:"));
  display.print(humidity, 0);
  display.println(F("%"));
  
  // Row 2: pH
  display.print(F("pH Volt: "));
  display.print(phVoltage, 2);
  display.print(F("V "));
  display.println(phTrigger ? F("[!]") : F("[OK]"));
  
  // Row 3: TDS
  display.print(F("TDS: "));
  display.print(tdsValue, 0);
  display.println(F(" ppm"));
  
  // Row 4: Light status
  display.print(F("Light: "));
  display.println(lightDetected ? F("BRIGHT") : F("DARK"));
  
  // Row 5: Divider
  display.println(F("----------------"));
  
  // Row 6: Connection status
  if (wifiConnected) {
    display.print(F("MQTT:"));
    display.print(client.connected() ? F("OK ") : F("-- "));
    display.println(WiFi.localIP());
  } else {
    display.println(F("** OFFLINE MODE **"));
  }
  
  display.display();
}

void readAllSensors() {
  // DHT11
  humidity = dht.readHumidity();
  temperature = dht.readTemperature(true); // Fahrenheit
  
  // pH sensor (with voltage divider compensation)
  phVoltage = analogRead(pinPo) * (3.3 / 4095.0) * 1.5;
  phTrigger = digitalRead(pinDo);
  
  // TDS sensor
  float tempC = (temperature - 32) * 5.0 / 9.0;
  tdsValue = readTDS(tempC);
  
  // Light sensor (digital only)
  lightDetected = (digitalRead(pinLightDigital) == LOW);
}

void loop() {
  unsigned long currentMillis = millis();
  
  // Retry WiFi connection periodically if offline
  if (!wifiConnected && (currentMillis - lastWifiRetry >= WIFI_RETRY_INTERVAL)) {
    lastWifiRetry = currentMillis;
    Serial.println("Retrying WiFi...");
    wifiConnected = connectWiFi();
    if (wifiConnected) {
      espClient.setInsecure();
      client.setServer(mqtt_server, 8883);
    }
  }
  
  // Handle MQTT if connected
  if (wifiConnected) {
    if (!client.connected()) reconnectMQTT();
    client.loop();
  }

  // Read all sensors
  readAllSensors();

// Update OLED at 500ms refresh rate
  if (currentMillis - lastOledUpdate >= OLED_PERIOD_MS) {
    lastOledUpdate = currentMillis;
    updateOLED();  // Always update, even if DHT fails
  }

  // Publish to MQTT every 5 seconds (only if connected)
  if (wifiConnected && client.connected() && (currentMillis - lastPublish >= PUBLISH_INTERVAL)) {
    lastPublish = currentMillis;
    
    if (isnan(humidity) || isnan(temperature)) {
      Serial.println("DHT read failed!");
      return;
    }

    // Build JSON payload
    String payload = "{";
    payload += "\"ambient\": {\"temp\": " + String(temperature) + ", \"hum\": " + String(humidity) + "},";
    payload += "\"hydro\": {\"waterTemp\": " + String(temperature) + ", \"ph\": " + String(phVoltage) + ", \"tds\": " + String(tdsValue) + "},";
    payload += "\"light\": {\"detected\": " + String(lightDetected ? "true" : "false") + "}";
    payload += "}";

    client.publish("hydro/telemetry", payload.c_str());
    Serial.println("Published: " + payload);
  }
  
  // Print to serial even when offline
  if (currentMillis - lastPublish >= PUBLISH_INTERVAL) {
    if (!wifiConnected) {
      lastPublish = currentMillis;
      Serial.print("T:");
      Serial.print(temperature, 1);
      Serial.print("F H:");
      Serial.print(humidity, 0);
      Serial.print("% pH:");
      Serial.print(phVoltage, 2);
      Serial.print("V TDS:");
      Serial.print(tdsValue, 0);
      Serial.print(" Light:");
      Serial.println(lightDetected ? "BRIGHT" : "DARK");
    }
  }
}