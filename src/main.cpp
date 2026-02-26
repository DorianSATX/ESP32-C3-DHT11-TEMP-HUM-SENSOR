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

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// Weather & UI Variables
float outdoorTemp = 0.0;
float outdoorHum = 0.0;
String weatherDesc = "Clear";
String sunriseTime = "--:--";
String sunsetTime = "--:--";
unsigned long lastWeatherUpdate = 0;
bool showOutdoor = false;
const char* state_topic = "home/sensor/esp32c3_dht11/state";

// Helper: Convert UTC string to Local Time (CST is UTC-6)
String convertToCST(String utcTime) {
    int hour = utcTime.substring(0, 2).toInt();
    int min = utcTime.substring(3, 5).toInt();
    
    hour = hour - 6; // Central Standard Time offset
    if (hour < 0) hour += 24;
    
    char buffer[10];
    sprintf(buffer, "%02d:%02d", hour, min);
    return String(buffer);
}

// --- 1. WiFi & MQTT (Skipped for brevity, keep your existing functions) ---
void setup_wifi() { /* Your existing code */ }
void reconnect() { /* Your existing code */ }

// --- 2. Enhanced Weather Fetch ---
void getOutdoorData() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        JsonDocument doc;

        // A. Forecast & Temp
        String pointsUrl = "https://api.weather.gov/points/" + String(LATITUDE) + "," + String(LONGITUDE);
        http.begin(pointsUrl);
        http.addHeader("User-Agent", USER_AGENT);
        if (http.GET() == 200) {
            deserializeJson(doc, http.getString());
            String forecastUrl = doc["properties"]["forecast"];
            http.end();
            
            http.begin(forecastUrl);
            http.addHeader("User-Agent", USER_AGENT);
            if (http.GET() == 200) {
                deserializeJson(doc, http.getString());
                outdoorTemp = doc["properties"]["periods"][0]["temperature"];
                weatherDesc = doc["properties"]["periods"][0]["shortForecast"].as<String>();
            }
        }
        http.end();

        // B. Humidity (Observations)
        http.begin("https://api.weather.gov/stations/KSAT/observations/latest");
        http.addHeader("User-Agent", USER_AGENT);
        if (http.GET() == 200) {
            deserializeJson(doc, http.getString());
            outdoorHum = doc["properties"]["relativeHumidity"]["value"];
        }
        http.end();

        // C. Sun Times
        String sunUrl = "https://api.sunrise-sunset.org/json?lat=" + String(LATITUDE) + "&lng=" + String(LONGITUDE) + "&formatted=0";
        http.begin(sunUrl);
        if (http.GET() == 200) {
            deserializeJson(doc, http.getString());
            sunriseTime = convertToCST(doc["results"]["sunrise"].as<String>().substring(11, 16));
            sunsetTime = convertToCST(doc["results"]["sunset"].as<String>().substring(11, 16));
        }
        http.end();
    }
}

// --- 3. Display Logic ---
void updateDisplay(float t_f, float h, bool mqtt) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    if (showOutdoor) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print("OUTSIDE (UTSA)");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

        display.setCursor(0, 15);
        display.setTextSize(2);
        display.printf("%.0f F", outdoorTemp);
        
        display.setTextSize(1);
        display.setCursor(75, 20);
        display.printf("H:%.0f%%", outdoorHum);

        display.setCursor(0, 36);
        display.printf("Sunrise: %s", sunriseTime);
        display.setCursor(0, 46);
        display.printf("Sunset:  %s", sunsetTime);

        display.setCursor(0, 57);
        display.print(weatherDesc);
    } else {
        // Indoor View (Same as before)
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print(mqtt ? "MQTT:OK" : "MQTT:ERR");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

        display.setCursor(0, 16);
        display.setTextSize(2);
        display.printf("IN: %.1f F", t_f);
        display.setCursor(0, 38);
        display.printf("Hum: %.0f%%", h);
        
        display.setTextSize(1);
        display.setCursor(0, 56);
        int rssi = WiFi.RSSI();
        display.printf("RSSI: %d dBm", rssi);
    }
    display.display();
}

void setup() {
    Serial.begin(115200);
    Wire.begin(8, 9);
    display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    dht.begin();
    setup_wifi();
    client.setServer(MQTT_SERVER, 1883);
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
}