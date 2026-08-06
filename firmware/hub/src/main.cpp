#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include "secrets.h"
#include "web_ui.h"

// Europe/Madrid (CET/CEST, EU DST rules).
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.nist.gov";
const char *MDNS_HOSTNAME = "plant-hub";

// Must match the satellite's hardcoded channel (see
// firmware/pump-satellite/include/config.h) — set explicitly so ESP-NOW
// works from boot, independent of whether the home router is reachable.
const int WIFI_CHANNEL = 1;

const int WATER_HOUR = 23;
const int WATER_MINUTE = 0;
const unsigned long SCHEDULE_CHECK_INTERVAL_MS = 30000;

// Measured 2026-08-06: 125mL over a 10s run (tube pre-primed), through
// ~80cm of tubing/head — matches the real deployment geometry, not a
// zero-head bench number. Single pump/satellite for now; will need to
// move to a per-satellite value in the hub registry once more than one
// pump exists (different pumps/tubing runs will vary).
const bool FLOW_RATE_CALIBRATED = true;
const float FLOW_RATE_ML_PER_SEC = 12.5;

int lastWateredDayIndex = -1;
unsigned long lastScheduleCheckMs = 0;

// After a power cut, the router/modem often isn't back yet by the time
// the hub reboots — these must never block satellite control or the web
// server, just retry quietly in the background. See tryConnectWifi().
bool wifiConnected = false;
unsigned long lastWifiAttemptMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;

bool timeSynced = false;
unsigned long lastTimeSyncAttemptMs = 0;
const unsigned long TIME_SYNC_RETRY_INTERVAL_MS = 30000;

WebServer server(80);

// --- ESP-NOW protocol (kept in sync by hand with
// firmware/pump-satellite/src/main.cpp — small enough that a shared header
// isn't worth it yet across two separate PlatformIO projects). ---
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

// Single unnamed satellite, no registry yet — a fresh check-in just
// overwrites this. Fine while there's exactly one satellite; will need a
// real MAC-keyed registry once a second one is cloned in.
bool satelliteKnown = false;
uint8_t satelliteMac[6];
String lastSatelliteMacStr = "";
unsigned long lastSatelliteMillis = 0;
uint32_t lastSatelliteCounter = 0;

bool hasReport = false;
unsigned long lastReportMillis = 0;
uint16_t lastReportRanMs = 0;

// Temporary raw diagnostics — records every ESP-NOW packet regardless of
// whether it parses, so we can see what's arriving from the web page
// without needing a serial connection.
uint32_t debugPacketsSeen = 0;
uint8_t debugLastType = 0;
int debugLastLen = -1;
String debugLastMac = "";
bool debugEspNowInitOk = false;
String debugHubMac = "";

// Wall-clock hour/minute/second, from real synced time when available or
// a free-running fallback (starts at 00:00:00 at boot) when not — see
// getCurrentTime(). dayIndex is only for the schedule's "already watered
// today" guard; it's not a real calendar day when unsynced, just a
// monotonic counter that changes once every 24h of uptime.
struct SimpleTime {
  int hour;
  int minute;
  int second;
  long dayIndex;
};

SimpleTime getCurrentTime() {
  SimpleTime t;
  if (timeSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
      t.hour = timeinfo.tm_hour;
      t.minute = timeinfo.tm_min;
      t.second = timeinfo.tm_sec;
      t.dayIndex = timeinfo.tm_yday;
      return t;
    }
  }
  unsigned long totalSec = millis() / 1000;
  t.dayIndex = (long)(totalSec / 86400);
  unsigned long secOfDay = totalSec % 86400;
  t.hour = secOfDay / 3600;
  t.minute = (secOfDay % 3600) / 60;
  t.second = secOfDay % 60;
  return t;
}

void ensurePeer(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 0;  // 0 = hub's current WiFi channel
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Failed to register satellite as ESP-NOW peer");
  }
}

void sendCommand(const uint8_t *mac, uint16_t runMs) {
  ensurePeer(mac);
  CommandMsg cmd{MSG_COMMAND, runMs};
  esp_err_t result = esp_now_send(mac, (uint8_t *)&cmd, sizeof(cmd));
  Serial.printf("Sent command: run %ums (%s)\n", runMs,
                result == ESP_OK ? "queued" : "error");
}

// mL -> ms using the flow-rate constant above. Uncalibrated readings are
// still sent (better to see it run than silently do nothing), just flagged
// in the serial log so a wrong dose is obvious while testing.
void waterMl(float ml) {
  if (!satelliteKnown) {
    Serial.println("Water request ignored: no satellite has checked in yet");
    return;
  }
  uint16_t runMs = (uint16_t)((ml / FLOW_RATE_ML_PER_SEC) * 1000.0);
  if (!FLOW_RATE_CALIBRATED) {
    Serial.println("(flow rate not calibrated yet — dose will be off)");
  }
  sendCommand(satelliteMac, runMs);
}

void waterNow() {
  // Placeholder dose until a real per-reservoir schedule/registry exists.
  waterMl(50.0);
}

void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  debugPacketsSeen++;
  debugLastLen = len;
  debugLastType = len >= 1 ? data[0] : 0;
  char dbgMac[18];
  snprintf(dbgMac, sizeof(dbgMac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  debugLastMac = dbgMac;

  if (len < 1) return;
  uint8_t type = data[0];

  if (type == MSG_CHECK_IN && len == sizeof(CheckInMsg)) {
    CheckInMsg msg;
    memcpy(&msg, data, sizeof(msg));

    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5]);
    memcpy(satelliteMac, mac, 6);
    satelliteKnown = true;
    lastSatelliteMacStr = macStr;
    lastSatelliteMillis = millis();
    lastSatelliteCounter = msg.counter;
    Serial.printf("Check-in #%u from %s\n", msg.counter, macStr);

  } else if (type == MSG_REPORT && len == sizeof(ReportMsg)) {
    ReportMsg msg;
    memcpy(&msg, data, sizeof(msg));
    hasReport = true;
    lastReportMillis = millis();
    lastReportRanMs = msg.ranMs;
    Serial.printf("Report: ran %ums\n", msg.ranMs);

  } else {
    Serial.printf("Ignored ESP-NOW message: type=%d len=%d\n", type, len);
  }
}

// Bounded, non-blocking-ish attempt: waits up to 8s for this specific
// attempt to succeed, then gives up and lets loop() retry later rather
// than blocking forever. Never called from a context that would delay
// ESP-NOW/satellite handling for more than that window. On success, also
// kicks off mDNS + the first NTP attempt.
void tryConnectWifi() {
  Serial.printf("Attempting WiFi connection to \"%s\"...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("WiFi connected. IP: %s, channel: %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.channel());
    if (MDNS.begin(MDNS_HOSTNAME)) {
      Serial.printf("mDNS started: http://%s.local\n", MDNS_HOSTNAME);
    } else {
      Serial.println("mDNS failed to start (IP address will still work)");
    }
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    lastTimeSyncAttemptMs = millis();
    Serial.println("Requested NTP sync");
  } else {
    Serial.println("WiFi connection attempt timed out, will retry later");
  }
}

void handleRoot() {
  SimpleTime now = getCurrentTime();
  char timeStr[9];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.hour, now.minute,
           now.second);

  String body;
  body += F("<h1>Sistema de riego</h1>");
  body += "<p class='stat-label'>" +
          String(timeSynced ? "Hora sincronizada"
                             : "Hora estimada (sin sincronizar)") +
          "</p>";
  body += "<p class='stat-value' id='clock'>" + String(timeStr) + "</p>";
  // Ticks the displayed clock forward locally every second so the page
  // doesn't look frozen between reloads — no server polling involved.
  body += F(
      "<script>"
      "(function(){"
      "var el=document.getElementById('clock');"
      "var p=el.textContent.split(':').map(Number);"
      "var s=p[0]*3600+p[1]*60+p[2];"
      "setInterval(function(){"
      "s=(s+1)%86400;"
      "var h=Math.floor(s/3600),m=Math.floor(s%3600/60),sec=s%60;"
      "function pad(n){return String(n).padStart(2,'0');}"
      "el.textContent=pad(h)+':'+pad(m)+':'+pad(sec);"
      "},1000);"
      "})();"
      "</script>");

  body += F("<p class='stat-label' style='margin-top:24px'>Último satélite</p>");
  if (!satelliteKnown) {
    body += F("<p class='stat-value' style='font-size:1.25rem'>(ninguno aún)</p>");
  } else {
    unsigned long secsAgo = (millis() - lastSatelliteMillis) / 1000;
    body += "<p class='stat-value' style='font-size:1.25rem'>" +
            lastSatelliteMacStr + " · hace " + String(secsAgo) + "s</p>";
  }

  body += F("<p class='stat-label' style='margin-top:24px'>Último riego</p>");
  if (!hasReport) {
    body += F("<p class='stat-value' style='font-size:1.25rem'>(ninguno aún)</p>");
  } else {
    unsigned long secsAgo = (millis() - lastReportMillis) / 1000;
    body += "<p class='stat-value' style='font-size:1.25rem'>" +
            String(lastReportRanMs) + " ms · hace " + String(secsAgo) +
            "s</p>";
  }

  body += "<p class='stat-label' style='margin-top:24px'>Debug ESP-NOW</p>"
          "<p class='stat-value' style='font-size:1rem'>wifi=" +
          String(wifiConnected ? "ok" : "esperando") + " horaSync=" +
          String(timeSynced ? "ok" : "no") + " init=" +
          String(debugEspNowInitOk ? "ok" : "FAIL") + " hubMac=" +
          debugHubMac + " canal=" + String(WiFi.channel()) + " vistos=" +
          String(debugPacketsSeen) + " tipo=" + String(debugLastType) +
          " len=" + String(debugLastLen) + " remitente=" +
          (debugLastMac.length() ? debugLastMac : "-") +
          " satCheckInCounter=" + String(lastSatelliteCounter) + "</p>";

  body += F(
      "<p class='stat-label' style='margin-top:24px'>Prueba de bomba "
      "(calibración)</p>"
      "<form action='/test' method='get' style='margin-top:4px'>"
      "<input type='number' name='seconds' value='3' min='1' max='30' "
      "style='width:60px'> segundos "
      "<button type='submit'>Probar</button>"
      "</form>");

  body += "<p class='stat-label' style='margin-top:24px'>Riego manual";
  if (!FLOW_RATE_CALIBRATED) body += " (sin calibrar)";
  body += F(
      "</p>"
      "<form action='/water' method='get' style='margin-top:4px'>"
      "<input type='number' name='ml' value='50' min='1' max='1000' "
      "style='width:70px'> mL "
      "<button type='submit'>Regar</button>"
      "</form>");

  server.send(200, "text/html", pageShell("Sistema de riego", body));
}

void handleTest() {
  int seconds = server.arg("seconds").toInt();
  if (seconds < 1) seconds = 1;
  if (satelliteKnown) {
    sendCommand(satelliteMac, seconds * 1000);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWater() {
  int ml = server.arg("ml").toInt();
  if (ml < 1) ml = 1;
  waterMl((float)ml);
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);

  // ESP-NOW/satellite control must work independent of the home router —
  // set STA mode and pin the shared channel before ever attempting to
  // join WiFi, so a slow-to-boot router after a power cut doesn't block
  // watering.
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  debugHubMac = WiFi.macAddress();
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    debugEspNowInitOk = false;
  } else {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.printf("ESP-NOW ready. Hub MAC: %s\n", WiFi.macAddress().c_str());
    debugEspNowInitOk = true;
  }

  server.on("/", handleRoot);
  server.on("/test", handleTest);
  server.on("/water", handleWater);
  server.begin();
  Serial.println("Web server started");

  // First WiFi attempt. If the router isn't up yet (e.g. right after a
  // power cut), this times out and loop() keeps retrying — everything
  // above already works without it.
  lastWifiAttemptMs = millis();
  tryConnectWifi();
}

void loop() {
  // Handled every iteration (not gated behind the schedule check below) so
  // the web server stays responsive instead of blocking on a sleep.
  server.handleClient();

  unsigned long nowMs = millis();

  if (!wifiConnected && nowMs - lastWifiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiAttemptMs = nowMs;
    tryConnectWifi();
  }

  if (wifiConnected && !timeSynced) {
    if (nowMs - lastTimeSyncAttemptMs >= TIME_SYNC_RETRY_INTERVAL_MS) {
      lastTimeSyncAttemptMs = nowMs;
      configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
      Serial.println("Retrying NTP sync");
    }
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
      timeSynced = true;
      Serial.printf("Time synced: %02d:%02d:%02d\n", timeinfo.tm_hour,
                    timeinfo.tm_min, timeinfo.tm_sec);
    }
  }

  if (nowMs - lastScheduleCheckMs >= SCHEDULE_CHECK_INTERVAL_MS) {
    lastScheduleCheckMs = nowMs;
    SimpleTime t = getCurrentTime();
    // Guard on dayIndex so the 1-minute match window can't fire twice.
    // Works even unsynced: dayIndex still advances once per 24h of
    // uptime, it just isn't tied to a real calendar date until synced.
    if (t.hour == WATER_HOUR && t.minute == WATER_MINUTE &&
        t.dayIndex != lastWateredDayIndex) {
      lastWateredDayIndex = t.dayIndex;
      waterNow();
    }
  }
}
