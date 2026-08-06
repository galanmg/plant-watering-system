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
  MSG_ACK = 4,
};
struct CheckInMsg {
  uint8_t type;
  uint32_t counter;
};
struct CommandMsg {
  uint8_t type;
  uint32_t commandId;
  uint32_t runMs;
};
struct ReportMsg {
  uint8_t type;
  uint32_t commandId;
  uint32_t ranMs;
};
struct AckMsg {
  uint8_t type;
  uint32_t commandId;
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
volatile uint32_t pendingCommandId = 0;
volatile uint32_t pendingRunMs = 0;

// Dedupes the hub's 2x-redundant command sends — see onEspNowRecv().
volatile bool hasSeenCommand = false;
volatile uint32_t lastSeenCommandId = 0;

// After a run, keep resending the report until the hub acks it (or we
// give up). Spaced out rather than a tight burst, and non-blocking — this
// can take minutes without holding up check-ins, which is fine since
// watering happens at most 3x/day and there are hours until the next one.
volatile bool awaitingAck = false;
volatile uint32_t awaitingCommandId = 0;
uint32_t awaitingRanMs = 0;
unsigned long lastReportSendMs = 0;
unsigned long reportRetryStartMs = 0;
const unsigned long REPORT_RETRY_INTERVAL_MS = 2000;
const unsigned long REPORT_RETRY_TIMEOUT_MS = 5UL * 60UL * 1000UL;

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
void runPump(uint32_t commandId, uint32_t runMs) {
  Serial.printf("Running pump for %ums (command #%u)\n", runMs, commandId);
  digitalWrite(RELAY_PIN, HIGH);
  delay(runMs);
  digitalWrite(RELAY_PIN, LOW);
  // Let any relay-switching electrical transient settle before keying the
  // radio.
  delay(200);

  awaitingCommandId = commandId;
  awaitingRanMs = runMs;
  lastReportSendMs = 0;  // forces an immediate send on the next loop()
  reportRetryStartMs = millis();
  awaitingAck = true;

  // A run >= the check-in interval would otherwise make loop() fire a
  // check-in immediately after — two back-to-back esp_now_send() calls in
  // the same iteration, which this SDK seems to drop rather than queue.
  // Reset the timer so the next check-in waits its normal full interval.
  lastCheckInMs = millis();
}

void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len < 1) return;
  uint8_t type = data[0];

  if (type == MSG_COMMAND && len == sizeof(CommandMsg)) {
    CommandMsg cmd;
    memcpy(&cmd, data, sizeof(cmd));
    // The hub sends each command 2x for reliability (same reasoning as
    // this satellite retrying its report) — without this dedupe, the
    // second copy arriving mid-run (easily possible: they're only 150ms
    // apart, runs are usually seconds long) would queue a second run
    // right after the first finishes.
    bool isDuplicate = hasSeenCommand && cmd.commandId == lastSeenCommandId;
    if (cmd.runMs > 0 && !isDuplicate) {
      hasSeenCommand = true;
      lastSeenCommandId = cmd.commandId;
      pendingCommandId = cmd.commandId;
      pendingRunMs = cmd.runMs;
      pendingRun = true;
    }
  } else if (type == MSG_ACK && len == sizeof(AckMsg)) {
    AckMsg ack;
    memcpy(&ack, data, sizeof(ack));
    if (awaitingAck && ack.commandId == awaitingCommandId) {
      awaitingAck = false;
      Serial.printf("Hub acked command #%u\n", ack.commandId);
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
    runPump(pendingCommandId, pendingRunMs);
  }

  unsigned long nowMs = millis();

  if (awaitingAck) {
    if (nowMs - reportRetryStartMs > REPORT_RETRY_TIMEOUT_MS) {
      Serial.printf("Gave up waiting for ack on command #%u\n",
                    awaitingCommandId);
      awaitingAck = false;
    } else if (nowMs - lastReportSendMs >= REPORT_RETRY_INTERVAL_MS) {
      lastReportSendMs = nowMs;
      ReportMsg report{MSG_REPORT, awaitingCommandId, awaitingRanMs};
      esp_err_t result =
          esp_now_send(HUB_MAC, (uint8_t *)&report, sizeof(report));
      Serial.printf("Report send for command #%u (%s)\n", awaitingCommandId,
                    result == ESP_OK ? "queued" : "error");
    }
  }

  if (nowMs - lastCheckInMs >= CHECK_IN_INTERVAL_MS) {
    lastCheckInMs = nowMs;
    sendCheckIn();
  }
}
