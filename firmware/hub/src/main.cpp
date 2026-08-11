#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
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

const unsigned long SCHEDULE_CHECK_INTERVAL_MS = 30000;
// A satellite that hasn't checked in within this window shows offline
// (grey) rather than online (green) — slack for missed 5s check-ins,
// since individual ESP-NOW sends occasionally just don't land. Widened
// 2026-08-11 after confirming with 3 satellites running concurrently
// that channel contention makes brief misses more frequent than with
// just one — was 20000 (4 missed check-ins), still real but false-flicker
// prone at that count once multiple satellites share the channel.
const unsigned long SATELLITE_OFFLINE_MS = 35000;

// Measured 2026-08-06: 125mL over a 10s run (tube pre-primed), through
// ~80cm of tubing/head — matches the real deployment geometry, not a
// zero-head bench number. Shared across satellites for now; move to a
// per-satellite value once different pumps/tubing runs need different
// numbers.
const bool FLOW_RATE_CALIBRATED = true;
const float FLOW_RATE_ML_PER_SEC = 12.5;

// Fixed duration for the "Probar" button — a quick sanity check, not
// meant to dose a real amount, so no point exposing it as an editable
// field.
const int TEST_RUN_SECONDS = 5;

unsigned long lastScheduleCheckMs = 0;

// After a power cut, the router/modem often isn't back yet by the time
// the hub reboots — these must never block satellite control or the web
// server, just retry quietly in the background. See tryConnectWifi().
// Also covers WiFi dropping *after* a successful connection (router
// reboots/renames/etc. while the hub keeps running on mains power) — see
// the periodic WiFi.status() check in loop().
bool wifiConnected = false;
unsigned long lastWifiAttemptMs = 0;
const unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
unsigned long lastWifiStatusCheckMs = 0;
const unsigned long WIFI_STATUS_CHECK_INTERVAL_MS = 10000;

bool timeSynced = false;
unsigned long lastTimeSyncAttemptMs = 0;
const unsigned long TIME_SYNC_RETRY_INTERVAL_MS = 30000;
// Defense against ESP32 clock drift over a multi-day/week trip — resync
// periodically even once already synced, not just once at boot.
unsigned long lastPeriodicResyncMs = 0;
const unsigned long PERIODIC_RESYNC_INTERVAL_MS = 6UL * 60UL * 60UL * 1000UL;

// Preventive daily reboot — clears any Arduino String heap fragmentation
// from hours of page loads/status polls before it can turn into an
// actual crash, rather than waiting to find out the hard way. 04:00 is
// picked to avoid typical watering windows; also skipped for a cycle if
// a satellite is mid-report, so it can never cut off an active watering.
long lastRebootDayIndex = -1;
const int REBOOT_HOUR = 4;
const int REBOOT_MINUTE = 0;

WebServer server(80);
Preferences prefs;

// --- ESP-NOW protocol (kept in sync by hand with
// firmware/pump-satellite/src/main.cpp — small enough that a shared header
// isn't worth it yet across two separate PlatformIO projects). ---
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

uint32_t nextCommandId = 1;

// Deferred send from the recv callback to loop() — never send from inside
// the callback itself, see firmware/pump-satellite's comment on the same
// issue.
struct PendingAck {
  bool pending;
  uint8_t mac[6];
  uint32_t commandId;
};

// --- Satellite registry — persisted to flash (NVS via Preferences) so
// dose/schedule survive a reboot. A satellite is auto-registered into the
// first free slot the moment its first check-in arrives; nothing here
// requires the web UI to "add" one manually. ---
const int MAX_SATELLITES = 4;
const int SLOTS_PER_SATELLITE = 5;
struct SatelliteConfig {
  bool inUse;
  uint8_t mac[6];
  char name[24];  // empty until renamed — falls back to "Satélite N"
  bool slotEnabled[SLOTS_PER_SATELLITE];
  uint8_t slotHour[SLOTS_PER_SATELLITE];
  uint8_t slotMinute[SLOTS_PER_SATELLITE];
  float slotDoseMl[SLOTS_PER_SATELLITE];  // independent per slot — more at
                                           // night, less at midday, etc.,
                                           // rather than one dose for all.
};
SatelliteConfig satellites[MAX_SATELLITES];
long lastWateredDayIndex[MAX_SATELLITES][SLOTS_PER_SATELLITE];

// Live status — not persisted, resets each boot (that's fine, it's
// "am I hearing from it right now", not configuration).
struct SatelliteRuntime {
  bool everSeen;
  unsigned long lastSeenMillis;
  uint32_t lastCounter;
  bool hasReport;
  unsigned long lastReportMillis;
  uint32_t lastReportRanMs;
  uint32_t lastReportCommandId;  // dedupes retried reports before our ack lands
  bool hasLastCommandId;
};
SatelliteRuntime satRuntime[MAX_SATELLITES];
PendingAck pendingAcks[MAX_SATELLITES];

// While a satellite is running the pump + retrying its report, it can't
// send check-ins (its loop() is blocked/busy) — so "last seen" naturally
// goes stale during a perfectly normal run. Without this, the status dot
// would misleadingly flip to offline right when a watering is in
// progress. Cleared the instant a report arrives; times out on its own
// if one never does, so a genuinely dead satellite still ends up showing
// offline rather than "waiting" forever.
bool awaitingReport[MAX_SATELLITES];
unsigned long commandSentMillis[MAX_SATELLITES];
const unsigned long AWAITING_REPORT_DISPLAY_TIMEOUT_MS = 90000;

// Short in-memory history per satellite — not the full 45-day flash log
// from architecture.md's Reservoir tracking (that's a separate, bigger
// feature), just enough to see recent activity on the page. Resets on
// reboot.
const int HISTORY_PER_SATELLITE = 10;
struct WateringEvent {
  bool valid;
  uint32_t commandId;
  uint32_t ranMs;
  int hour, minute;  // wall-clock (or fallback clock) at report time
  bool wasSynced;    // whether hour/minute above is real time or fallback
};
WateringEvent history[MAX_SATELLITES][HISTORY_PER_SATELLITE];
int historyNext[MAX_SATELLITES];

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
// getCurrentTime(). dayIndex is only for the "already watered this slot
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

void addHistory(int satIdx, uint32_t commandId, uint32_t ranMs) {
  SimpleTime t = getCurrentTime();
  int slot = historyNext[satIdx];
  history[satIdx][slot] = {true, commandId, ranMs, t.hour, t.minute,
                            timeSynced};
  historyNext[satIdx] = (slot + 1) % HISTORY_PER_SATELLITE;
}

void loadSatelliteConfigs() {
  prefs.begin("satcfg", true);
  size_t len = prefs.getBytesLength("cfg");
  if (len == sizeof(satellites)) {
    prefs.getBytes("cfg", satellites, sizeof(satellites));
  } else {
    memset(satellites, 0, sizeof(satellites));
  }
  prefs.end();
}

void saveSatelliteConfigs() {
  prefs.begin("satcfg", false);
  prefs.putBytes("cfg", satellites, sizeof(satellites));
  prefs.end();
}

int findSatelliteIndex(const uint8_t *mac) {
  for (int i = 0; i < MAX_SATELLITES; i++) {
    if (satellites[i].inUse && memcmp(satellites[i].mac, mac, 6) == 0) {
      return i;
    }
  }
  return -1;
}

// Returns the satellite's registry index, registering it with default
// (disabled) schedule slots on first-ever check-in.
int registerSatellite(const uint8_t *mac) {
  int idx = findSatelliteIndex(mac);
  if (idx >= 0) return idx;

  for (int i = 0; i < MAX_SATELLITES; i++) {
    if (!satellites[i].inUse) {
      satellites[i].inUse = true;
      memcpy(satellites[i].mac, mac, 6);
      satellites[i].name[0] = '\0';
      for (int s = 0; s < SLOTS_PER_SATELLITE; s++) {
        satellites[i].slotEnabled[s] = false;
        satellites[i].slotHour[s] = 23;
        satellites[i].slotMinute[s] = 0;
        satellites[i].slotDoseMl[s] = 50.0;
      }
      saveSatelliteConfigs();
      Serial.printf("Registered new satellite in slot %d\n", i);
      return i;
    }
  }
  Serial.println("Satellite registry full — ignoring new satellite");
  return -1;
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

void sendCommand(const uint8_t *mac, uint32_t runMs) {
  ensurePeer(mac);
  uint32_t commandId = nextCommandId++;
  CommandMsg cmd{MSG_COMMAND, commandId, runMs};
  // Sent 2x — cheap insurance against the same occasional dropped-packet
  // issue the report side has; the satellite dedupes retried reports by
  // commandId so a duplicate command is harmless (just runs once, since
  // the satellite only starts a new run instruction, not accumulates).
  for (int i = 0; i < 2; i++) {
    esp_err_t result = esp_now_send(mac, (uint8_t *)&cmd, sizeof(cmd));
    Serial.printf("Sent command #%u: run %ums (%s)\n", commandId, runMs,
                  result == ESP_OK ? "queued" : "error");
    delay(150);
  }

  int idx = findSatelliteIndex(mac);
  if (idx >= 0) {
    awaitingReport[idx] = true;
    commandSentMillis[idx] = millis();
  }
}

uint32_t mlToRunMs(float ml) {
  return (uint32_t)((ml / FLOW_RATE_ML_PER_SEC) * 1000.0);
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
    int idx = registerSatellite(mac);
    if (idx >= 0) {
      satRuntime[idx].everSeen = true;
      satRuntime[idx].lastSeenMillis = millis();
      satRuntime[idx].lastCounter = msg.counter;
    }
    Serial.printf("Check-in #%u from %s (slot %d)\n", msg.counter, dbgMac,
                  idx);

  } else if (type == MSG_REPORT && len == sizeof(ReportMsg)) {
    ReportMsg msg;
    memcpy(&msg, data, sizeof(msg));
    int idx = findSatelliteIndex(mac);
    if (idx >= 0) {
      // The satellite retries this send until it hears our ack, so the
      // same commandId can arrive more than once — only record history
      // once per commandId, but always re-queue the ack (ours may have
      // been the one that got dropped).
      bool isNew = !satRuntime[idx].hasLastCommandId ||
                   satRuntime[idx].lastReportCommandId != msg.commandId;
      satRuntime[idx].hasReport = true;
      satRuntime[idx].lastReportMillis = millis();
      satRuntime[idx].lastReportRanMs = msg.ranMs;
      satRuntime[idx].lastReportCommandId = msg.commandId;
      satRuntime[idx].hasLastCommandId = true;
      awaitingReport[idx] = false;
      if (isNew) {
        addHistory(idx, msg.commandId, msg.ranMs);
      }
      pendingAcks[idx].pending = true;
      memcpy(pendingAcks[idx].mac, mac, 6);
      pendingAcks[idx].commandId = msg.commandId;
    }
    Serial.printf("Report from %s: command #%u ran %ums\n", dbgMac,
                  msg.commandId, msg.ranMs);

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

String macToString(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

String satelliteDisplayName(int idx) {
  if (satellites[idx].name[0] != '\0') return String(satellites[idx].name);
  return "Satélite " + String(idx + 1);
}

// Estimated mL for a given run duration, using the shared calibrated flow
// rate — the whole point being that this is what a person reads, not a
// duration they'd have to mentally convert (and which means a different
// amount on a different pump anyway).
float runMsToMl(uint32_t runMs) {
  return (runMs / 1000.0) * FLOW_RATE_ML_PER_SEC;
}

// Shared by the full-page render and /status.json — walks satellite i's
// history ring buffer newest-to-oldest and hands each valid entry to cb.
template <typename Cb>
void forEachHistoryEntry(int i, Cb cb) {
  for (int h = 0; h < HISTORY_PER_SATELLITE; h++) {
    int slot = (historyNext[i] - 1 - h + 2 * HISTORY_PER_SATELLITE) %
               HISTORY_PER_SATELLITE;
    if (!history[i][slot].valid) continue;
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", history[i][slot].hour,
              history[i][slot].minute);
    cb(String(timeBuf), history[i][slot].wasSynced,
       (int)round(runMsToMl(history[i][slot].ranMs)));
  }
}

// Shared by the full-page render and the /status.json poll endpoint, so
// the two can never disagree about what "online" means.
struct SatStatusDisplay {
  String dotClass;
  String label;
  String lastSeenText;
  String lastWaterText;
};

// "hace Ns" only reads well for the first minute — past that, scale up
// to the largest sensible unit (minutes, then hours, then days) rather
// than showing a raw, hard-to-parse second count.
String formatAgo(unsigned long ageSec) {
  if (ageSec < 60) return "hace " + String(ageSec) + "s";
  if (ageSec < 3600) return "hace " + String(ageSec / 60) + " min";
  if (ageSec < 86400) return "hace " + String(ageSec / 3600) + " h";
  return "hace " + String(ageSec / 86400) + " d";
}

SatStatusDisplay computeSatStatus(int i) {
  SatStatusDisplay d;
  unsigned long age = millis() - satRuntime[i].lastSeenMillis;
  bool freshCheckIn = satRuntime[i].everSeen && age <= SATELLITE_OFFLINE_MS;
  bool waiting = awaitingReport[i] &&
                 (millis() - commandSentMillis[i]) <=
                     AWAITING_REPORT_DISPLAY_TIMEOUT_MS;
  // Fault (red, --status-critical) is reserved for a future dry-run/
  // current-sensing alarm — nothing sets it yet.
  if (waiting) {
    d.dotClass = "dot-waiting";
    d.label = "Esperando confirmación";
  } else if (freshCheckIn) {
    d.dotClass = "dot-online";
    d.label = "En línea";
  } else {
    d.dotClass = "dot-offline";
    d.label = "Sin conexión";
  }

  d.lastSeenText = satRuntime[i].everSeen ? formatAgo(age / 1000)
                                           : "(ninguna aún)";

  if (satRuntime[i].hasReport) {
    unsigned long rAge = (millis() - satRuntime[i].lastReportMillis) / 1000;
    d.lastWaterText =
        "~" + String((int)round(runMsToMl(satRuntime[i].lastReportRanMs))) +
        " mL · " + formatAgo(rAge);
  } else {
    d.lastWaterText = "(ninguno aún)";
  }
  return d;
}

void renderSatellites(String &body) {
  bool any = false;
  for (int i = 0; i < MAX_SATELLITES; i++) {
    if (!satellites[i].inUse) continue;
    any = true;

    SatStatusDisplay st = computeSatStatus(i);

    body += "<div class='card sat-card'>";
    body += "<p class='sat-title'><span class='status-dot " + st.dotClass +
            "' id='dot-" + String(i) + "'></span>" + satelliteDisplayName(i) +
            " — <span id='label-" + String(i) + "'>" + st.label + "</span></p>";

    body += "<p class='sat-meta' id='lastseen-" + String(i) +
            "'>Última conexión: " + st.lastSeenText + "</p>";
    body += "<p class='sat-meta' id='lastwater-" + String(i) +
            "'>Último riego: " + st.lastWaterText + "</p>";

    int activeSlots = 0;
    float totalMl = 0;
    for (int s = 0; s < SLOTS_PER_SATELLITE; s++) {
      if (satellites[i].slotEnabled[s]) {
        activeSlots++;
        totalMl += satellites[i].slotDoseMl[s];
      }
    }
    body += "<p class='sat-meta' style='margin-top:8px'>Riegos activos: " +
            String(activeSlots) + " · Total al día: " +
            String((int)totalMl) + " mL</p>";

    body += F("<details><summary>Editar horario</summary>");
    body += "<form action='/satellite/save' method='get'>";
    body += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    for (int s = 0; s < SLOTS_PER_SATELLITE; s++) {
      char timeVal[6];
      snprintf(timeVal, sizeof(timeVal), "%02d:%02d", satellites[i].slotHour[s],
                satellites[i].slotMinute[s]);
      body += "<div class='slot-row'><input type='checkbox' name='e" +
              String(s) + "'" +
              (satellites[i].slotEnabled[s] ? " checked" : "") +
              "> Horario " + String(s + 1) +
              " <input type='time' name='t" + String(s) + "' value='" +
              timeVal + "'>"
              " <input type='number' name='ml" + String(s) + "' value='" +
              String((int)satellites[i].slotDoseMl[s]) +
              "' min='1' max='5000' style='width:90px'> mL</div>";
    }
    body += "<button type='submit'>Guardar horario</button>";
    body += F("</form></details>");

    body += F("<div class='action-row'>");
    body += "<form action='/satellite/water' method='get'>";
    body += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    body +=
        "<div class='slot-row'><input type='number' name='ml' value='50' "
        "min='1' max='5000' style='width:90px'> mL "
        "<button type='submit'>Regar ahora</button></div>";
    body += "</form>";

    body += "<form action='/satellite/test' method='get'>";
    body += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    body += F("<button type='submit'>Probar</button>");
    body += "</form>";
    body += F("</div>");

    String rowsHtml;
    forEachHistoryEntry(i, [&](String t, bool synced, int ml) {
      rowsHtml += "<tr><td>" + t + (synced ? "" : "*") + "</td><td>~" +
                  String(ml) + " mL</td></tr>";
    });

    body += F("<div class='button-row'>");
    body += F("<details><summary>Ajustes</summary>");
    body += "<form action='/satellite/rename' method='get'>";
    body += "<input type='hidden' name='idx' value='" + String(i) + "'>";
    body += "<div class='slot-row'>Nombre <input type='text' name='name' value='" +
            satelliteDisplayName(i) + "' maxlength='23' style='width:140px'></div>";
    body += "<button type='submit'>Guardar nombre</button>";
    body += "</form>";
    body += "<p class='sat-meta' style='margin-top:8px'>MAC: " +
            macToString(satellites[i].mac) + "</p>";
    body += F("</details>");
    // Always rendered, even with zero entries — so it's not there one
    // reload and gone the next; the poll (see below) fills it in live
    // once the first watering happens, no manual reload needed.
    body += "<details><summary>Historial reciente</summary>"
            "<table id='history-" + String(i) + "'><tr><th>Hora</th><th>Cantidad</th></tr>";
    body += rowsHtml;
    body += F("</table></details>");
    body += F("</div>");

    body += "</div>";
  }

  if (!any) {
    body += F(
        "<div class='card'><p class='stat-label'>Satélites</p>"
        "<p class='stat-value' style='font-size:1.25rem'>(ninguno aún)</p>"
        "</div>");
  }
}

void handleRoot() {
  SimpleTime now = getCurrentTime();
  char timeStr[9];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", now.hour, now.minute,
           now.second);

  String body;
  body.reserve(3500);

  // The whole card is a plain link to "/" — tapping it anywhere just
  // reloads the page. Easier to hit than the browser's own tiny refresh
  // button, especially on mobile.
  body += F("<a href='/' class='card header-card'>");
  body += F("<h1>Sistema de riego</h1>");
  body += "<p class='stat-label'>" +
          String(timeSynced ? "Hora sincronizada"
                             : "Hora estimada (sin sincronizar)") +
          "</p>";
  body += "<p class='stat-value' id='clock'>" + String(timeStr) + "</p>";
  body += F("</a>");
  // Ticks the displayed clock forward locally every second so the page
  // doesn't look frozen between reloads — no server polling involved.
  // Kept outside the <a> above (script isn't valid link content).
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

  renderSatellites(body);

  body += "<details><summary>Debug ESP-NOW</summary><p class='sat-meta'>wifi=" +
          String(wifiConnected ? "ok" : "esperando") + " horaSync=" +
          String(timeSynced ? "ok" : "no") + " init=" +
          String(debugEspNowInitOk ? "ok" : "FAIL") + " heap=" +
          String(ESP.getFreeHeap()) + "B hubMac=" + debugHubMac +
          " canal=" + String(WiFi.channel()) + " vistos=" +
          String(debugPacketsSeen) + " tipo=" + String(debugLastType) +
          " len=" + String(debugLastLen) + " remitente=" +
          (debugLastMac.length() ? debugLastMac : "-") + "</p></details>";

  // Polls /status.json every 5s and patches only the status dot/label/
  // last-seen/last-water text by id — never touches the schedule forms,
  // so it can't interrupt someone mid-edit.
  body += F(
      "<script>"
      "setInterval(function(){"
      "fetch('/status.json').then(function(r){return r.json();})"
      ".then(function(data){"
      "data.satellites.forEach(function(s){"
      "var dot=document.getElementById('dot-'+s.idx);"
      "if(dot)dot.className='status-dot '+s.dotClass;"
      "var label=document.getElementById('label-'+s.idx);"
      "if(label)label.textContent=s.label;"
      "var seen=document.getElementById('lastseen-'+s.idx);"
      "if(seen)seen.textContent='Última conexión: '+s.lastSeenText;"
      "var water=document.getElementById('lastwater-'+s.idx);"
      "if(water)water.textContent='Último riego: '+s.lastWaterText;"
      "var hist=document.getElementById('history-'+s.idx);"
      "if(hist)hist.innerHTML='<tr><th>Hora</th><th>Cantidad</th></tr>'+s.historyRows;"
      "});"
      "}).catch(function(){});"
      "},5000);"
      "</script>");

  server.send(200, "text/html", pageShell("Sistema de riego", body));
}

// Polled by the page's own JS every few seconds to refresh just the
// status dot/label/last-seen/last-water text — not a full page reload, so
// it never disturbs an in-progress schedule edit. All the interpolated
// text here is server-computed (numbers, fixed labels), never user input,
// so no JSON escaping is needed.
// Manual test hook: simulates the router vanishing (reboots, ISP hiccup,
// etc.) without needing actual router access — same effect from the
// hub's point of view as the AP disappearing, since it only ever looks
// at WiFi.status(). Not linked from the UI, hit it directly when needed.
void handleDebugWifiDisconnect() {
  Serial.println("[debug] Forcing WiFi disconnect to simulate router loss");
  WiFi.disconnect();
  server.send(200, "text/plain",
              "WiFi disconnected. Watch the debug line — should show "
              "wifi=esperando within ~10s, then reconnect on its own.");
}

void handleStatusJson() {
  String json = "{\"satellites\":[";
  bool first = true;
  for (int i = 0; i < MAX_SATELLITES; i++) {
    if (!satellites[i].inUse) continue;
    if (!first) json += ",";
    first = false;
    SatStatusDisplay st = computeSatStatus(i);
    String rowsHtml;
    forEachHistoryEntry(i, [&](String t, bool synced, int ml) {
      rowsHtml += "<tr><td>" + t + (synced ? "" : "*") + "</td><td>~" +
                  String(ml) + " mL</td></tr>";
    });
    rowsHtml.replace("\"", "\\\"");
    json += "{\"idx\":" + String(i) + ",\"dotClass\":\"" + st.dotClass +
            "\",\"label\":\"" + st.label + "\",\"lastSeenText\":\"" +
            st.lastSeenText + "\",\"lastWaterText\":\"" + st.lastWaterText +
            "\",\"historyRows\":\"" + rowsHtml + "\"}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleSatelliteRename() {
  int idx = server.arg("idx").toInt();
  if (idx >= 0 && idx < MAX_SATELLITES && satellites[idx].inUse) {
    String name = server.arg("name");
    name.trim();
    strncpy(satellites[idx].name, name.c_str(), sizeof(satellites[idx].name) - 1);
    satellites[idx].name[sizeof(satellites[idx].name) - 1] = '\0';
    saveSatelliteConfigs();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSatelliteSave() {
  int idx = server.arg("idx").toInt();
  if (idx >= 0 && idx < MAX_SATELLITES && satellites[idx].inUse) {
    for (int s = 0; s < SLOTS_PER_SATELLITE; s++) {
      satellites[idx].slotEnabled[s] = server.hasArg("e" + String(s));
      String t = server.arg("t" + String(s));
      int h = satellites[idx].slotHour[s];
      int m = satellites[idx].slotMinute[s];
      int colon = t.indexOf(':');
      if (colon > 0) {
        h = t.substring(0, colon).toInt();
        m = t.substring(colon + 1).toInt();
      }
      satellites[idx].slotHour[s] = constrain(h, 0, 23);
      satellites[idx].slotMinute[s] = constrain(m, 0, 59);

      float ml = server.arg("ml" + String(s)).toFloat();
      satellites[idx].slotDoseMl[s] = ml < 1 ? 1 : ml;

      // The "already watered today" guard is keyed by slot number, not by
      // the slot's configured time — without this reset, editing a slot
      // that already fired earlier today (e.g. while calibrating/testing
      // a schedule) would silently suppress the new time for the rest of
      // the day, with no indication why. Saving always makes every slot
      // eligible again today.
      lastWateredDayIndex[idx][s] = -1;
    }
    saveSatelliteConfigs();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSatelliteTest() {
  int idx = server.arg("idx").toInt();
  if (idx >= 0 && idx < MAX_SATELLITES && satellites[idx].inUse) {
    sendCommand(satellites[idx].mac, TEST_RUN_SECONDS * 1000);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSatelliteWater() {
  int idx = server.arg("idx").toInt();
  float ml = server.arg("ml").toFloat();
  if (ml < 1) ml = 1;
  if (idx >= 0 && idx < MAX_SATELLITES && satellites[idx].inUse) {
    sendCommand(satellites[idx].mac, mlToRunMs(ml));
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void setup() {
  Serial.begin(115200);

  loadSatelliteConfigs();
  for (int i = 0; i < MAX_SATELLITES; i++) {
    for (int s = 0; s < SLOTS_PER_SATELLITE; s++) lastWateredDayIndex[i][s] = -1;
  }

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
  server.on("/status.json", handleStatusJson);
  server.on("/debug/wifi-disconnect", handleDebugWifiDisconnect);
  server.on("/satellite/rename", handleSatelliteRename);
  server.on("/satellite/save", handleSatelliteSave);
  server.on("/satellite/test", handleSatelliteTest);
  server.on("/satellite/water", handleSatelliteWater);
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

  // Deferred from onEspNowRecv() — never send from inside that callback.
  for (int i = 0; i < MAX_SATELLITES; i++) {
    if (!pendingAcks[i].pending) continue;
    pendingAcks[i].pending = false;
    ensurePeer(pendingAcks[i].mac);
    AckMsg ack{MSG_ACK, pendingAcks[i].commandId};
    esp_now_send(pendingAcks[i].mac, (uint8_t *)&ack, sizeof(ack));
    Serial.printf("Acked command #%u\n", pendingAcks[i].commandId);
  }

  // Detects WiFi dropping *after* a successful connection — e.g. the
  // router reboots, gets renamed, or otherwise vanishes while the hub
  // stays powered. Without this, `wifiConnected` would stay stuck true
  // forever (it's only ever set once, on initial connect) and the retry
  // logic below would never fire again even though the link is actually
  // down. Time keeps working regardless (the ESP32's own clock, once
  // synced, doesn't depend on an active WiFi link), and ESP-NOW/satellite
  // control was never gated on WiFi in the first place — this check only
  // restores the *web page* and lets NTP resync once the network's back.
  if (wifiConnected &&
      nowMs - lastWifiStatusCheckMs >= WIFI_STATUS_CHECK_INTERVAL_MS) {
    lastWifiStatusCheckMs = nowMs;
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("WiFi connection lost — will retry in background");
    }
  }

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
      lastPeriodicResyncMs = nowMs;
      Serial.printf("Time synced: %02d:%02d:%02d\n", timeinfo.tm_hour,
                    timeinfo.tm_min, timeinfo.tm_sec);
    }
  }

  // Periodic resync even once already synced — cheap insurance against
  // the ESP32's internal clock drifting over a multi-day/week trip.
  if (wifiConnected && timeSynced &&
      nowMs - lastPeriodicResyncMs >= PERIODIC_RESYNC_INTERVAL_MS) {
    lastPeriodicResyncMs = nowMs;
    configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    Serial.println("Periodic NTP resync");
  }

  // A report that never arrives (satellite gave up retrying after 5min)
  // would otherwise leave this flag stuck true forever — harmless today
  // since the status display already has its own timeout, but tidied up
  // at the data level so the flag actually means what it says.
  for (int i = 0; i < MAX_SATELLITES; i++) {
    if (awaitingReport[i] &&
        nowMs - commandSentMillis[i] > AWAITING_REPORT_DISPLAY_TIMEOUT_MS) {
      awaitingReport[i] = false;
    }
  }

  if (nowMs - lastScheduleCheckMs >= SCHEDULE_CHECK_INTERVAL_MS) {
    lastScheduleCheckMs = nowMs;
    SimpleTime t = getCurrentTime();

    // The uptime floor matters: lastRebootDayIndex resets to -1 on every
    // boot (it's not persisted), so without it a reboot that happens to
    // land back in the 04:00 minute (very plausible — boot+reconnect+
    // resync can take under a minute) would immediately trigger another
    // one, forever. 5 minutes safely clears any realistic boot time
    // while staying far short of the ~24h real cadence.
    if (millis() > 5UL * 60UL * 1000UL && t.hour == REBOOT_HOUR &&
        t.minute == REBOOT_MINUTE && lastRebootDayIndex != t.dayIndex) {
      bool anyAwaitingReport = false;
      for (int i = 0; i < MAX_SATELLITES; i++) {
        if (awaitingReport[i]) anyAwaitingReport = true;
      }
      if (anyAwaitingReport) {
        Serial.println(
            "Skipping scheduled reboot this cycle — satellite mid-report");
      } else {
        Serial.println("Scheduled daily reboot");
        delay(100);  // let the serial line flush before restarting
        ESP.restart();
      }
    }

    for (int i = 0; i < MAX_SATELLITES; i++) {
      if (!satellites[i].inUse) continue;
      for (int s = 0; s < SLOTS_PER_SATELLITE; s++) {
        if (!satellites[i].slotEnabled[s]) continue;
        // Guard on dayIndex so the 1-minute match window can't fire
        // twice. Works even unsynced: dayIndex still advances once per
        // 24h of uptime, it just isn't tied to a real calendar date
        // until synced.
        if (t.hour == satellites[i].slotHour[s] &&
            t.minute == satellites[i].slotMinute[s] &&
            lastWateredDayIndex[i][s] != t.dayIndex) {
          lastWateredDayIndex[i][s] = t.dayIndex;
          sendCommand(satellites[i].mac, mlToRunMs(satellites[i].slotDoseMl[s]));
        }
      }
    }
  }
}
