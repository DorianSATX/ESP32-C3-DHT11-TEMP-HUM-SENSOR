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

// --- Configuration ---
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

// --- Weather & UI Variables ---
float outdoorTemp = 0.0;
float outdoorHum = 0.0;
String weatherDesc = "Updating...";
String sunriseTime = "--:--";
String sunsetTime = "--:--";
unsigned long lastWeatherUpdate = 0;
bool showOutdoor = false;
const char* state_topic = "home/sensor/esp32c3_dht11/state";

// --- Helper: UTC ISO String to Local AM/PM ---
String formatTime(String timeStr) {
    if (timeStr.length() < 5) return "--:--";
    int hour = timeStr.substring(0, 2).toInt();
    int min = timeStr.substring(3, 5).toInt();
    
    // CST Offset is UTC -6.
    hour = hour - 6; 
    if (hour < 0) hour += 24;
    
    int displayHour = (hour == 0 || hour == 12) ? 12 : hour % 12;
    String ampm = (hour >= 12) ? "PM" : "AM";
    
    char buffer[10];
    sprintf(buffer, "%d:%02d%s", displayHour, min, ampm.c_str());
    return String(buffer);
}

// --- WiFi Setup ---
void setup_wifi() {
    delay(10);
    int retry = 0;
    Serial.printf("\nWiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500); Serial.print("."); retry++;
    }
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.begin(WIFI_BACKUP_SSID, WIFI_BACKUP_PASS);
        retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 20) {
            delay(500); Serial.print("."); retry++;
        }
    }
    if (WiFi.status() == WL_CONNECTED) Serial.println("\nWiFi OK!");
}

// --- MQTT Reconnect ---
void reconnect() {
    if (!client.connected()) {
        if (client.connect("ESP32C3_WeatherStation", MQTT_USER, MQTT_PASS)) {
            Serial.println("MQTT OK");
        }
    }
}

// --- 3. THE CLEANED UP WEATHER FETCH ---
void getOutdoorData() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    JsonDocument doc;
    String payload;

    // A. NWS Forecast (Temp, Desc, and BACKUP Humidity)
    String pointsUrl = "https://api.weather.gov/points/" + String(LATITUDE) + "," + String(LONGITUDE);
    http.begin(pointsUrl);
    http.addHeader("User-Agent", USER_AGENT);
    if (http.GET() == 200) {
        payload = http.getString();
        deserializeJson(doc, payload);
        String forecastUrl = doc["properties"]["forecast"].as<String>();
        http.end(); 

        if (forecastUrl != "") {
            doc.clear();
            http.begin(forecastUrl);
            http.addHeader("User-Agent", USER_AGENT);
            if (http.GET() == 200) {
                payload = http.getString();
                deserializeJson(doc, payload);
                outdoorTemp = doc["properties"]["periods"][0]["temperature"];
                weatherDesc = doc["properties"]["periods"][0]["shortForecast"].as<String>();
                
                // Set initial humidity from forecast
                outdoorHum = doc["properties"]["periods"][0]["relativeHumidity"]["value"];
            }
            http.end();
        }
    }

  // B. Humidity (Attempt Primary Station: KSAT)
    doc.clear();
    http.begin("https://api.weather.gov/stations/KSAT/observations/latest");
    http.addHeader("User-Agent", USER_AGENT);
    if (http.GET() == 200) {
        payload = http.getString();
        deserializeJson(doc, payload);
        
        // --- FIX HERE: Use JsonVariant instead of auto ---
        JsonVariant stationHum = doc["properties"]["relativeHumidity"]["value"];
        
        if (!stationHum.isNull()) {
            outdoorHum = stationHum.as<float>();
            Serial.println("Humidity from Station: OK");
        }
    }
    http.end();

    // C. Sunrise/Sunset
    doc.clear();
    String sunUrl = "https://api.sunrise-sunset.org/json?lat=" + String(LATITUDE) + "&lng=" + String(LONGITUDE) + "&formatted=0";
    http.begin(sunUrl);
    if (http.GET() == 200) {
        payload = http.getString();
        deserializeJson(doc, payload);
        String riseRaw = doc["results"]["sunrise"].as<String>();
        String setRaw = doc["results"]["sunset"].as<String>();
        
        if (riseRaw.indexOf('T') != -1) {
            sunriseTime = formatTime(riseRaw.substring(riseRaw.indexOf('T') + 1, riseRaw.indexOf('T') + 6));
            sunsetTime = formatTime(setRaw.substring(setRaw.indexOf('T') + 1, setRaw.indexOf('T') + 6));
        }
    }
    http.end();
}

// --- Display & Core ---
void updateDisplay(float t_f, float h, bool mqtt) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    if (showOutdoor) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print("OUTSIDE (NWS)");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

        display.setCursor(0, 15);
        display.setTextSize(2);
        display.printf("  %.0f F", outdoorTemp);
        
        display.setTextSize(1);
        display.setCursor(75, 20);
        display.printf("H:%.0f%%", outdoorHum);

        display.setCursor(0, 36);
        display.printf("Sunrise: %s", sunriseTime.c_str());
        display.setCursor(0, 46);
        display.printf("Sunset:  %s", sunsetTime.c_str());

        display.setCursor(0, 57);
        display.print(weatherDesc.length() > 20 ? weatherDesc.substring(0, 20) : weatherDesc);
    } else {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print(mqtt ? "MQTT: OK" : "MQTT: ERR");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

        display.setCursor(0, 16);
        display.setTextSize(2);
        display.printf("I: %.1f F", t_f);
        display.setCursor(0, 38);
        display.printf("H: %.0f%%", h);
        
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.printf("RSSI: %d dBm", WiFi.RSSI());
    }
    display.display();
}

void setup() {
    Serial.begin(115200);
    Wire.begin(8, 9);
    display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    dht.begin();
    setup_wifi();
    ArduinoOTA.begin();
    client.setServer(MQTT_SERVER, MQTT_PORT);
    getOutdoorData();
}

void loop() {
    ArduinoOTA.handle();
    if (!client.connected()) reconnect();
    client.loop();

    static unsigned long lastRotate = 0;
    if (millis() - lastRotate > 5000) {
        lastRotate = millis();
        showOutdoor = !showOutdoor;
        updateDisplay(dht.readTemperature(true), dht.readHumidity(), client.connected());
    }

    if (millis() - lastWeatherUpdate > 900000 || lastWeatherUpdate == 0) {
        lastWeatherUpdate = millis();
        getOutdoorData();
    }

    static unsigned long lastMsg = 0;
    if (millis() - lastMsg > 30000) {
        lastMsg = millis();
        float t = dht.readTemperature(true);
        float h = dht.readHumidity();
        if (!isnan(t)) {
            char p[128];
            snprintf(p, 128, "{\"temperature\": %.1f, \"humidity\": %.1f}", t, h);
            client.publish(state_topic, p);
        }
    }
}