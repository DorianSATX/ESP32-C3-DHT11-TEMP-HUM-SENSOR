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

// --- MQTT Topics ---
const char* state_topic = "home/sensor/esp32c3_dht11/state";
const char* dim_topic = "home/sensor/esp32c3_dht11/display/set";
const char* hum_config_topic = "homeassistant/sensor/c3_hum/config";
const char* temp_config_topic = "homeassistant/sensor/c3_temp/config";

// --- Weather & UI Variables ---
float outdoorTemp = 0.0;
float outdoorHum = 0.0;
String weatherDesc = "Updating...";
String sunriseTime = "--:--";
String sunsetTime = "--:--";
unsigned long lastWeatherUpdate = 0;
int screenState = 0;  // 0 = Indoor, 1 = Outdoor, 2 = Sun/Forecast

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

void sendDiscovery() {
    if (client.connected()) {
        String humPayload = "{\"name\":\"C3 Hum\",\"stat_t\":\"" + String(state_topic) + "\",\"unit_of_meas\":\"%\",\"dev_cla\":\"humidity\",\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.humidity}}\",\"uniq_id\":\"c3_hum_01\"}";
        client.publish(hum_config_topic, humPayload.c_str(), true);
        String tempPayload = "{\"name\":\"C3 Temp\",\"stat_t\":\"" + String(state_topic) + "\",\"unit_of_meas\":\"F\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\",\"val_tpl\":\"{{value_json.temperature}}\",\"uniq_id\":\"c3_temp_01\"}";
        client.publish(temp_config_topic, tempPayload.c_str(), true);
        Serial.println("MQTT Discovery Sent");
    }
}

void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (int i = 0; i < length; i++) { message += (char)payload[i]; }
    if (String(topic) == dim_topic) {
        display.dim(message == "OFF");
    }
}

void setup_wifi() {
    delay(10);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) { delay(500); retry++; }
    if (WiFi.status() == WL_CONNECTED) Serial.println("\nWiFi OK!");
}

void reconnect() {
    while (!client.connected()) {
        if (client.connect("ESP32C3_WeatherStation", MQTT_USER, MQTT_PASS)) {
            client.subscribe(dim_topic);
            sendDiscovery();
        } else {
            delay(5000);
        }
    }
}

// --- IMPROVED Weather Fetch ---
void getOutdoorData() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    JsonDocument doc;

    // 1. Get Forecast for the text description (e.g. "Sunny")
    String pointsUrl = "https://api.weather.gov/points/" + String(LATITUDE) + "," + String(LONGITUDE);
    http.begin(pointsUrl);
    http.addHeader("User-Agent", USER_AGENT);
    if (http.GET() == 200) {
        deserializeJson(doc, http.getString());
        String forecastUrl = doc["properties"]["forecast"].as<String>();
        http.end(); 
        if (forecastUrl != "") {
            http.begin(forecastUrl);
            http.addHeader("User-Agent", USER_AGENT);
            if (http.GET() == 200) {
                doc.clear();
                deserializeJson(doc, http.getString());
                weatherDesc = doc["properties"]["periods"][0]["shortForecast"].as<String>();
            }
            http.end();
        }
    }

    // 2. Get REAL-TIME Observations from KSAT (Actual Temp and Hum)
    doc.clear();
    http.begin("https://api.weather.gov/stations/KSAT/observations/latest");
    http.addHeader("User-Agent", USER_AGENT);
    if (http.GET() == 200) {
        deserializeJson(doc, http.getString());
        
        // Temperature (NWS returns Celsius, we convert to Fahrenheit)
        JsonVariant stationTempC = doc["properties"]["temperature"]["value"];
        if (!stationTempC.isNull()) {
            outdoorTemp = (stationTempC.as<float>() * 9.0 / 5.0) + 32.0;
        }

        // Humidity
        JsonVariant stationHum = doc["properties"]["relativeHumidity"]["value"];
        if (!stationHum.isNull()) outdoorHum = stationHum.as<float>();
    }
    http.end();

    // 3. Sunrise/Sunset
    doc.clear();
    http.begin("https://api.sunrise-sunset.org/json?lat=" + String(LATITUDE) + "&lng=" + String(LONGITUDE) + "&formatted=0");
    if (http.GET() == 200) {
        deserializeJson(doc, http.getString());
        String riseRaw = doc["results"]["sunrise"].as<String>();
        String setRaw = doc["results"]["sunset"].as<String>();
        if (riseRaw.indexOf('T') != -1) {
            sunriseTime = formatTime(riseRaw.substring(riseRaw.indexOf('T') + 1, riseRaw.indexOf('T') + 6));
            sunsetTime = formatTime(setRaw.substring(setRaw.indexOf('T') + 1, setRaw.indexOf('T') + 6));
        }
    }
    http.end();
    Serial.println("Weather Updated from Station Observations");
}

void updateDisplay(float t_f, float h, bool mqtt) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);

    if (screenState == 0) {
        // --- SCREEN 1: INDOOR ---
        display.setCursor(0, 0);
        display.printf("%s %s .%d", WiFi.status() == WL_CONNECTED ? "W:OK" : "W:ER", mqtt ? "M:OK" : "M:ER", WiFi.localIP()[3]);
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
        display.setCursor(0, 16);
        display.setTextSize(2);
        display.printf("I: %.1f F", t_f);
        display.setCursor(0, 38);
        display.printf("H: %.0f%%", h);
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.printf("RSSI: %d dBm", WiFi.RSSI());

    } else if (screenState == 1) {
        // --- SCREEN 2: OUTSIDE (Large Format) ---
        display.setCursor(0, 0);
        display.print("OUTSIDE (NWS)");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
        display.setCursor(0, 16);
        display.setTextSize(2);
        display.printf("O: %.0f F", outdoorTemp);
        display.setCursor(0, 38);
        display.printf("H: %.0f%%", outdoorHum);
        display.setTextSize(1);
        display.setCursor(0, 56);
        display.print("Real-time Station Data");

    } else if (screenState == 2) {
        // --- SCREEN 3: FORECAST & SUN ---
        display.setCursor(0, 0);
        display.print("FORECAST & SUN");
        display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
        
        display.setCursor(0, 16);
        display.setTextSize(1);
        display.print("Sky:");
        display.setCursor(0, 26);
        display.setTextSize(1);
        // Word wrap for long descriptions
        display.println(weatherDesc); 
        
        display.drawLine(0, 42, 128, 42, SSD1306_WHITE);
        display.setCursor(0, 47);
        display.printf("Rise: %s", sunriseTime.c_str());
        display.setCursor(0, 56);
        display.printf("Set:  %s", sunsetTime.c_str());
    }
    
    display.display();
}

void setup() {
    Serial.begin(115200);
    Wire.begin(8, 9);
    display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
    dht.begin();
    setup_wifi();
    // --- OTA Configuration ---
    ArduinoOTA.setHostname("esp32-c3-DHT11air-sensor"); // Name that shows up on your network
   // ArduinoOTA.setPassword("your_password");    // Optional: add a password for security
   
   ArduinoOTA.onStart([]() {
    Serial.println("OTA Starting...");
    display.clearDisplay();
    display.setCursor(0, 20);
    display.print("OTA UPDATE...");
    display.display();
  });

  ArduinoOTA.begin();
    client.setServer(MQTT_SERVER, MQTT_PORT);
    client.setCallback(callback); // Set the listener for dimming commands
    getOutdoorData();
}

void loop() {
    ArduinoOTA.handle();
    if (!client.connected()) reconnect();
    client.loop();

    static unsigned long lastRotate = 0;
    if (millis() - lastRotate > 5000) {
        lastRotate = millis();
        
        screenState++;        // Move to next screen
        if (screenState > 2) screenState = 0; // Reset to first screen
        
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