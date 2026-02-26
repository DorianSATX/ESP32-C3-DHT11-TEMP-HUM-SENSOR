#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "DHT.h"
#include "secrets.h" 

// --- Settings ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C 
#define DHTPIN 4     
#define DHTTYPE DHT11

// --- Global Objects ---
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// Weather & UI Variables
float outdoorTemp = 0.0;
unsigned long lastWeatherUpdate = 0;
bool showOutdoor = false;
const char* state_topic = "home/sensor/esp32c3_dht11/state";

// --- 1. WiFi Setup (with Failover) ---
void setup_wifi() {
    delay(10);
    int retry_count = 0;
    const int max_retries = 20;

    Serial.printf("\nConnecting to Primary: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED && retry_count < max_retries) {
        delay(500); Serial.print("."); retry_count++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("\nTrying Backup: %s", WIFI_BACKUP_SSID);
        WiFi.begin(WIFI_BACKUP_SSID, WIFI_BACKUP_PASS);
        retry_count = 0;
        while (WiFi.status() != WL_CONNECTED && retry_count < max_retries) {
            delay(500); Serial.print("."); retry_count++;
        }
    }
    if (WiFi.status() == WL_CONNECTED) Serial.println("\nWiFi OK!");
}

// --- 2. MQTT Reconnect ---
void reconnect() {
    if (!client.connected()) {
        if (client.connect("ESP32C3_TempSensor", MQTT_USER, MQTT_PASS)) {
            // Re-send discovery if needed
        }
    }
}

// --- 3. Weather Fetch (NWS API) ---
void getOutdoorTemp() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = "https://api.weather.gov/points/" + String(LATITUDE) + "," + String(LONGITUDE);
        http.begin(url);
        http.addHeader("User-Agent", USER_AGENT);
        http.addHeader("X-Api-Key", X_API_KEY);
        
        if (http.GET() == 200) {
            JsonDocument doc;
            deserializeJson(doc, http.getString());
            String forecastUrl = doc["properties"]["forecast"];
            http.end();

            http.begin(forecastUrl);
            http.addHeader("User-Agent", USER_AGENT);
            http.addHeader("X-Api-Key", X_API_KEY);
            if (http.GET() == 200) {
                deserializeJson(doc, http.getString());
                outdoorTemp = doc["properties"]["periods"][0]["temperature"];
            }
        }
        http.end();
    }
}

// --- 4. Display Logic ---
void updateDisplay(float t_f, float h, bool mqtt) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print(mqtt ? "MQTT:OK" : "MQTT:ERR");
    
    int rssi = WiFi.RSSI();
    display.setCursor(85, 0);
    display.printf("%d dBm", rssi);
    display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

    if (showOutdoor) {
        display.setCursor(0, 16);
        display.println("LOCAL OUTSIDE:");
        display.setCursor(15, 35);
        display.setTextSize(2);
        display.printf("%.0f F", outdoorTemp);
    } else {
        display.setCursor(0, 16);
        display.println("ROOM INSIDE:");
        display.setCursor(0, 30);
        display.setTextSize(2);
        display.printf("T:%.1f F", t_f);
        display.setCursor(0, 48);
        display.printf("H:%.0f%%", h);
    }
    display.display();
}

// --- 5. Required Arduino Functions ---
void setup() {
    Serial.begin(115200);
    Wire.begin(8, 9);
    display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    dht.begin();
    
    setup_wifi();
    ArduinoOTA.begin();
    client.setServer(MQTT_SERVER, 1883);
    getOutdoorTemp();
}

void loop() {
    ArduinoOTA.handle();
    if (!client.connected()) { reconnect(); }
    client.loop();

    static unsigned long lastRotate = 0;
    if (millis() - lastRotate > 5000) {
        lastRotate = millis();
        showOutdoor = !showOutdoor;
        updateDisplay(dht.readTemperature(true), dht.readHumidity(), client.connected());
    }

    if (millis() - lastWeatherUpdate > 900000 || lastWeatherUpdate == 0) {
        lastWeatherUpdate = millis();
        getOutdoorTemp();
    }

    static unsigned long lastMsg = 0;
    if (millis() - lastMsg > 30000) { 
        lastMsg = millis();
        float t = dht.readTemperature(true);
        float h = dht.readHumidity();
        if (!isnan(t)) {
            char payload[128];
            snprintf(payload, 128, "{\"temperature\": %.1f, \"humidity\": %.1f}", t, h);
            client.publish(state_topic, payload);
        }
    }
}