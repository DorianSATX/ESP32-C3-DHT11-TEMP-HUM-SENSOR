#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include "DHT.h"
#include "secrets.h" 

// --- Display Settings ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1 
#define SCREEN_ADDRESS 0x3C 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Sensor Settings ---
#define DHTPIN 4     
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// --- Network Objects ---
WiFiClient espClient;
PubSubClient client(espClient);

// --- MQTT Topics ---
const char* state_topic = "home/sensor/esp32c3_dht11/state";
const char* temp_config_topic = "homeassistant/sensor/esp32c3_temp/config";
const char* hum_config_topic = "homeassistant/sensor/esp32c3_hum/config";

// --- Function: Home Assistant Discovery (Updated to Fahrenheit) ---
void send_discovery_config() {
    char temp_payload[256];
    // Changed unit_of_meas to °F
    snprintf(temp_payload, 256, 
        "{\"name\": \"C3 Temp\", \"stat_t\": \"%s\", \"unit_of_meas\": \"°F\", \"val_tpl\": \"{{ value_json.temperature }}\", \"dev_cla\": \"temperature\", \"uniq_id\": \"c3_temp_01\"}", 
        state_topic);
    client.publish(temp_config_topic, temp_payload, true);

    char hum_payload[256];
    snprintf(hum_payload, 256, 
        "{\"name\": \"C3 Hum\", \"stat_t\": \"%s\", \"unit_of_meas\": \"%%\", \"val_tpl\": \"{{ value_json.humidity }}\", \"dev_cla\": \"humidity\", \"uniq_id\": \"c3_hum_01\"}", 
        state_topic);
    client.publish(hum_config_topic, hum_payload, true);
}

// --- Function: WiFi Setup ---
void setup_wifi() {
    delay(10);
    Serial.printf("\nConnecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
}

// --- Function: MQTT Reconnect ---
void reconnect() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect("ESP32C3_TempSensor", MQTT_USER, MQTT_PASS)) {
            Serial.println("connected");
            send_discovery_config();
        } else {
            Serial.printf("failed, rc=%d. Try again in 5s\n", client.state());
            delay(5000);
        }
    }
}

// --- Function: Update OLED (Fahrenheit) ---
void updateDisplay(float t, float h, bool mqtt) {
    display.clearDisplay();
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.print(mqtt ? "MQTT: ONLINE" : "MQTT: OFFLINE");
    display.drawLine(0, 11, 128, 11, SSD1306_WHITE);

    // Temperature (F)
    display.setCursor(0, 20);
    display.setTextSize(2);
    display.printf("T: %.1f F", t);

    // Humidity
    display.setCursor(0, 45);
    display.printf("H: %.0f %%", h);

    display.display();
}

// --- Main Setup ---
void setup() {
    Serial.begin(115200);

    // Initialize I2C (SDA=8, SCL=9 for ESP32-C3)
    Wire.begin(8, 9);

    if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;); 
    }
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(25, 25);
    display.println("BOOTING...");
    display.display();

    dht.begin();
    setup_wifi(); // Compiler now knows what this is!
    ArduinoOTA.begin();
    client.setServer(MQTT_SERVER, 1883);
}

// --- Main Loop ---
void loop() {
    ArduinoOTA.handle();
    if (!client.connected()) {
        reconnect(); // Compiler now knows what this is!
    }
    client.loop();

    static unsigned long lastMsg = 0;
    if (millis() - lastMsg > 30000) { 
        lastMsg = millis();
        float h = dht.readHumidity();
        // readTemperature(true) returns Fahrenheit
        float t = dht.readTemperature(true); 

        if (!isnan(h) && !isnan(t)) {
            char payload[128];
            snprintf(payload, 128, "{\"temperature\": %.1f, \"humidity\": %.1f}", t, h);
            client.publish(state_topic, payload);
            
            updateDisplay(t, h, client.connected());
            Serial.printf("Published (F): %s\n", payload);
        }
    }
}