#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "config.h"

// --- ESP-NOW protocol (kept in sync by hand with
// firmware/hub/src/main.cpp — small enough that a shared header isn't
// worth it yet across two separate PlatformIO projects). ---
enum : uint8_t {
  MSG_CHECK_IN = 1,
  MSG_COMMAND = 2,
  MSG_REPORT = 3,
};
struct CheckInMsg {
  uint8_t type;
  uint32_t counter;
};
struct CommandMsg {
  uint8_t type;
  uint16_t runMs;
};
struct ReportMsg {
  uint8_t type;
  uint16_t ranMs;
};

const int RELAY_PIN = 26;

uint32_t checkInCounter = 0;
unsigned long lastCheckInMs = 0;
const unsigned long CHECK_IN_INTERVAL_MS = 5000;

// Set by the ESP-NOW recv callback, consumed by loop(). The callback must
// stay fast (no delay(), no esp_now_send()) — it runs in the WiFi driver's
// own context, and blocking or sending from inside it can wedge the
// ESP-NOW/WiFi stack. Actually running the pump is deferred to loop().
volatile bool pendingRun = false;
volatile uint16_t pendingRunMs = 0;

void sendCheckIn() {
  checkInCounter++;
  CheckInMsg msg{MSG_CHECK_IN, checkInCounter};
  esp_err_t result = esp_now_send(HUB_MAC, (uint8_t *)&msg, sizeof(msg));
  Serial.printf("Check-in #%u (%s)\n", checkInCounter,
                result == ESP_OK ? "queued" : "error");
}

// Blocking on purpose: v1 runs the satellite awake full-time (no deep
// sleep, see architecture.md's Power section), and there's nothing else
// this board needs to do mid-run. Safe to block here — called from
// loop(), not from the ESP-NOW callback.
void runPump(uint16_t runMs) {
  Serial.printf("Running pump for %ums\n", runMs);
  digitalWrite(RELAY_PIN, HIGH);
  delay(runMs);
  digitalWrite(RELAY_PIN, LOW);
  // Let any relay-switching electrical transient settle before keying the
  // radio — suspected cause of the report send silently not landing.
  delay(200);

  ReportMsg report{MSG_REPORT, runMs};
  // Sent 3x with a short gap — ESP-NOW delivery isn't guaranteed, and this
  // is cheap insurance against an occasional dropped packet.
  for (int i = 0; i < 3; i++) {
    esp_err_t result =
        esp_now_send(HUB_MAC, (uint8_t *)&report, sizeof(report));
    Serial.printf("Report send attempt %d (%s)\n", i,
                  result == ESP_OK ? "queued" : "error");
    delay(150);
  }

  // Runs >= the check-in interval would otherwise make loop() immediately
  // fire a check-in right after these report sends — two back-to-back
  // esp_now_send() calls in the same loop iteration, which this SDK seems
  // to drop rather than queue. Reset the timer so the next check-in waits
  // its normal full interval instead.
  lastCheckInMs = millis();
}

void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < 1) return;
  uint8_t type = data[0];

  if (type == MSG_COMMAND && len == sizeof(CommandMsg)) {
    CommandMsg cmd;
    memcpy(&cmd, data, sizeof(cmd));
    if (cmd.runMs > 0) {
      pendingRunMs = cmd.runMs;
      pendingRun = true;
    }
  } else {
    Serial.printf("Ignored ESP-NOW message: type=%d len=%d\n", type, len);
  }
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
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, HUB_MAC, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to add hub as ESP-NOW peer");
  }
}

void loop() {
  if (pendingRun) {
    pendingRun = false;
    runPump(pendingRunMs);
  }

  unsigned long nowMs = millis();
  if (nowMs - lastCheckInMs >= CHECK_IN_INTERVAL_MS) {
    lastCheckInMs = nowMs;
    sendCheckIn();
  }
}
