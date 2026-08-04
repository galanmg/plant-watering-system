#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_now.h>
#include <time.h>
#include "secrets.h"
#include "web_ui.h"

// Europe/Madrid (CET/CEST, EU DST rules).
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

const char *NTP_SERVER_1 = "pool.ntp.org";
const char *NTP_SERVER_2 = "time.nist.gov";
const char *MDNS_HOSTNAME = "plant-hub";

const int WATER_HOUR = 23;
const int WATER_MINUTE = 0;
const unsigned long SCHEDULE_CHECK_INTERVAL_MS = 30000;

int lastWateredDayOfYear = -1;
unsigned long lastScheduleCheckMs = 0;

WebServer server(80);

// Satellite check-in state — v0: single unnamed satellite, no registry yet.
// A fresh satellite message just overwrites this; enough to prove the
// ESP-NOW link works end to end before building out a real registry.
struct SatelliteHello {
  uint32_t counter;
};
String lastSatelliteMac = "";
unsigned long lastSatelliteMillis = 0;
uint32_t lastSatelliteCounter = 0;

void connectWiFi() {
  Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected. IP: %s, channel: %d\n",
                WiFi.localIP().toString().c_str(), WiFi.channel());
}

void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != sizeof(SatelliteHello)) return;
  SatelliteHello msg;
  memcpy(&msg, data, sizeof(msg));

  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  lastSatelliteMac = macStr;
  lastSatelliteMillis = millis();
  lastSatelliteCounter = msg.counter;
  Serial.printf("ESP-NOW hello #%u from %s\n", msg.counter, macStr);
}

void syncTime() {
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  Serial.print("Waiting for NTP time sync...");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 5000)) {
    Serial.print(".");
  }
  Serial.printf("\nTime synced: %02d:%02d:%02d\n", timeinfo.tm_hour,
                timeinfo.tm_min, timeinfo.tm_sec);
}

void waterNow() {
  // Placeholder until satellites exist: this is where the hub will send an
  // ESP-NOW "run" command to each pump satellite. For now it just logs so
  // the scheduling logic itself can be verified end to end.
  Serial.println(">>> WATERING TRIGGERED <<<");
}

void handleRoot() {
  struct tm timeinfo;
  char timeStr[9] = "--:--:--";
  if (getLocalTime(&timeinfo, 1000)) {
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  }

  String body;
  body += F("<h1>Sistema de riego</h1>");
  body += F("<p class='stat-label'>Hora sincronizada</p>");
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
  if (lastSatelliteMac.length() == 0) {
    body += F("<p class='stat-value' style='font-size:1.25rem'>(ninguno aún)</p>");
  } else {
    unsigned long secsAgo = (millis() - lastSatelliteMillis) / 1000;
    body += "<p class='stat-value' style='font-size:1.25rem'>" +
            lastSatelliteMac + " · hace " + String(secsAgo) + "s</p>";
  }

  server.send(200, "text/html", pageShell("Sistema de riego", body));
}

void setup() {
  Serial.begin(115200);
  connectWiFi();
  syncTime();

  if (MDNS.begin(MDNS_HOSTNAME)) {
    Serial.printf("mDNS started: http://%s.local\n", MDNS_HOSTNAME);
  } else {
    Serial.println("mDNS failed to start (IP address will still work)");
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
  } else {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.printf("ESP-NOW ready. Hub MAC: %s\n",
                  WiFi.macAddress().c_str());
  }

  server.on("/", handleRoot);
  server.begin();
  Serial.println("Web server started");
}

void loop() {
  // Handled every iteration (not gated behind the schedule check below) so
  // the web server stays responsive instead of blocking on a sleep.
  server.handleClient();

  unsigned long nowMs = millis();
  if (nowMs - lastScheduleCheckMs >= SCHEDULE_CHECK_INTERVAL_MS) {
    lastScheduleCheckMs = nowMs;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 1000)) {
      // Guard on day-of-year so the 1-minute match window can't fire twice.
      if (timeinfo.tm_hour == WATER_HOUR && timeinfo.tm_min == WATER_MINUTE &&
          timeinfo.tm_yday != lastWateredDayOfYear) {
        lastWateredDayOfYear = timeinfo.tm_yday;
        waterNow();
      }
    }
  }
}
