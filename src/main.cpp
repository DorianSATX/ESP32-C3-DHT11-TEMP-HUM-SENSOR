#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include "DHT.h"
#include "secrets.h" 

#define DHTPIN 4     
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

// Discovery Topics for Home Assistant
const char* temp_config_topic = "homeassistant/sensor/esp32c3_temp/config";
const char* hum_config_topic = "homeassistant/sensor/esp32c3_hum/config";
const char* state_topic = "home/sensor/esp32c3_dht11/state";

void send_discovery_config() {
    // Temperature Config
    char temp_payload[256];
    snprintf(temp_payload, 256, 
        "{\"name\": \"C3 Temperature\", \"stat_t\": \"%s\", \"unit_of_meas\": \"°C\", \"val_tpl\": \"{{ value_json.temperature }}\", \"dev_cla\": \"temperature\", \"stat_cla\": \"measurement\", \"uniq_id\": \"c3_temp_01\"}", 
        state_topic);
    client.publish(temp_config_topic, temp_payload, true);

    // Humidity Config
    char hum_payload[256];
    snprintf(hum_payload, 256, 
        "{\"name\": \"C3 Humidity\", \"stat_t\": \"%s\", \"unit_of_meas\": \"%%\", \"val_tpl\": \"{{ value_json.humidity }}\", \"dev_cla\": \"humidity\", \"stat_cla\": \"measurement\", \"uniq_id\": \"c3_hum_01\"}", 
        state_topic);
    client.publish(hum_config_topic, hum_payload, true);
    
    Serial.println("Discovery configs sent!");
}

void setup_wifi() {
    delay(10);
    Serial.printf("\nConnecting to %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected! IP: ");
    Serial.println(WiFi.localIP());
}

void setup_ota() {
    ArduinoOTA.begin();
}

void reconnect() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection...");
        if (client.connect("ESP32C3_TempSensor", MQTT_USER, MQTT_PASS)) {
            Serial.println("connected");
            send_discovery_config(); // Send discovery right after connecting
        } else {
            Serial.printf("failed, rc=%d. Try again in 5s\n", client.state());
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    dht.begin();
    setup_wifi();
    setup_ota();
    client.setServer(MQTT_SERVER, 1883);
}

void loop() {
    ArduinoOTA.handle();
    if (!client.connected()) {
        reconnect();
    }
    client.loop();

    static unsigned long lastMsg = 0;
    if (millis() - lastMsg > 30000) { 
        lastMsg = millis();
        float h = dht.readHumidity();
        float t = dht.readTemperature();

        if (!isnan(h) && !isnan(t)) {
            char payload[128];
            snprintf(payload, 128, "{\"temperature\": %.1f, \"humidity\": %.1f}", t, h);
            client.publish(state_topic, payload);
            Serial.printf("Published: %s\n", payload);
        }
    }
}