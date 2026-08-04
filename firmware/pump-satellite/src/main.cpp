#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"

struct SatelliteHello {
  uint32_t counter;
};

uint32_t helloCounter = 0;
unsigned long lastSendMs = 0;
const unsigned long SEND_INTERVAL_MS = 3000;

// --- Temporary one-shot pump test — confirmed relay is active-HIGH.
// Runs the pump once, 5s after boot, for 3s. To be replaced with a real
// run-on-command handler once this confirms water actually moves. ---
const int RELAY_PIN = 26;
const unsigned long PUMP_TEST_START_MS = 5000;
const unsigned long PUMP_TEST_DURATION_MS = 3000;
bool pumpTestStarted = false;
bool pumpTestFinished = false;

void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.printf("Send status: %s\n",
                status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.printf("Satellite MAC: %s\n", WiFi.macAddress().c_str());

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // Never joins the router — only needs a fixed channel to talk ESP-NOW
  // directly to the hub.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, HUB_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add hub as ESP-NOW peer");
  }
}

void loop() {
  unsigned long nowMs = millis();

  if (!pumpTestStarted && nowMs >= PUMP_TEST_START_MS) {
    pumpTestStarted = true;
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Pump test: ON");
  }
  if (pumpTestStarted && !pumpTestFinished &&
      nowMs >= PUMP_TEST_START_MS + PUMP_TEST_DURATION_MS) {
    pumpTestFinished = true;
    digitalWrite(RELAY_PIN, LOW);
    Serial.println("Pump test: OFF (done)");
  }

  if (nowMs - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = nowMs;
    helloCounter++;
    SatelliteHello msg{helloCounter};
    esp_err_t result =
        esp_now_send(HUB_MAC, (uint8_t *)&msg, sizeof(msg));
    Serial.printf("Sending hello #%u (%s)\n", helloCounter,
                  result == ESP_OK ? "queued" : "error");
  }
}
