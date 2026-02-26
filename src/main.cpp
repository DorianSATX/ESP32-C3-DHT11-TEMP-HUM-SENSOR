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
String weatherDesc = "Loading...";
String sunriseTime = "--:--";
String sunsetTime = "--:--";
unsigned long lastWeatherUpdate = 0;
bool showOutdoor = false;
const char* state_topic = "home/sensor/esp32c3_dht11/state";

// --- Helper: UTC to CST Conversion ---
String formatTime(String isoTime) {
    if (isoTime.length() < 5) return "--:--";
    int hour = isoTime.substring(0, 2).toInt();
    int min = isoTime.substring(3, 5).toInt();
    
    // Central Standard Time Offset (-6)
    hour = hour - 6; 
    if (hour < 0) hour += 24;
    
    char buffer[10];
    int displayHour = (hour == 0 || hour == 12) ? 12 : hour % 12;
    String ampm = (hour >= 12) ? "PM" : "AM";
    
    sprintf(buffer, "%d:%02d%s", displayHour, min, ampm.c_str());
    return String(buffer);
}

// --- WiFi Setup with Backup Failover ---
void setup_wifi() {
    delay(10);
    int retry = 0;
    
    Serial.printf("\nConnecting to Primary: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500); Serial.print("."); retry++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("\nTrying Backup: %s", WIFI_BACKUP_SSID);
        WiFi.begin(WIFI_BACKUP_SSID, WIFI_BACKUP_PASS);
        retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 20) {
            delay(500); Serial.print("."); retry++;
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected");
    }
}

void reconnect() {
    if (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect("ESP32C3_WeatherStation", MQTT_USER, MQTT_PASS)) {
            Serial.println("connected");
        } else {
            Serial.printf("failed, rc=%d. Try again in 5s\n", client.state());
        }
    }
}

void getOutdoorData() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    JsonDocument doc; 

    // 1. NWS Forecast
    String pointsUrl = "https://api.weather.gov/points/" + String(LATITUDE) + "," + String(LONGITUDE);
    http.begin(pointsUrl);
    http.addHeader("User-Agent", USER_AGENT);
    http.addHeader("X-Api-Key", X_API_KEY);
    
    if (http.GET() == 200) {
        deserializeJson(doc, http.getStream());
        String forecastUrl = doc["properties"]["forecast"].as<String>();
        http.end(); 

        if (forecastUrl.length() > 0) {
            http.begin(forecastUrl);
            http.addHeader("User-Agent", USER_AGENT);
            if (http.GET() == 200) {
                doc.clear();
                deserializeJson(doc, http.getStream());
                outdoorTemp = doc["properties"]["periods"][0]["temperature"];
                weatherDesc = doc["properties"]["periods"][0]["shortForecast"].as<String>();
            }
            http.end();
        }
    } else {
        http.end();
    }

    // 2. Humidity
    doc.clear();
    http.begin("https://api.weather.gov/stations/KSAT/observations/latest");
    http.addHeader("User-Agent", USER_AGENT);
    if (http.GET() == 200) {
        deserializeJson(doc, http.getStream());
        outdoorHum = doc["properties"]["relativeHumidity"]["value"];
    }
    http.end();

    // 3. Sunrise/Sunset
    doc.clear();
    String sunUrl = "https://api.sunrise-sunset.org/json?lat=" + String(LATITUDE) + "&lng=" + String(LONGITUDE) + "&formatted=0";
    http.begin(sunUrl);
    if (http.GET() == 200) {
        deserializeJson(doc, http.getStream());
        String rawRise = doc["results"]["sunrise"].as<String>().substring(11, 16);
        String rawSet = doc["results"]["sunset"].as<String>().substring(11, 16);
        sunriseTime = formatTime(rawRise);
        sunsetTime = formatTime(rawSet);
    }
    http.end();
}

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
        display.printf("%.0f F", outdoorTemp);
        
        display.setTextSize(1);
        display.setCursor(75, 20);
        display.printf("H:%.0f%%", outdoorHum);

        display.setCursor(0, 36);
        display.printf("Rise: %s", sunriseTime.c_str());
        display.setCursor(0, 46);
        display.printf("Set:  %s", sunsetTime.c_str());

        display.setCursor(0, 57);
        display.print(weatherDesc.length() > 20 ? weatherDesc.substring(0, 20) : weatherDesc);
    } else {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.print(mqtt ? "MQTT: OK" : "MQTT: ERR");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

        display.setCursor(0, 16);
        display.setTextSize(2);
        display.printf("IN: %.1f F", t_f);
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
    
    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        for(;;);
    }
    
    display.clearDisplay();
    display.setCursor(0, 20);
    display.setTextColor(WHITE);
    display.print("Booting IoT Station...");
    display.display();

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
}