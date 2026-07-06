# Architecture

Status: draft, nothing built yet. This doc is meant to be updated as decisions
get made — treat "Open questions" as the running to-do list.

## Overview

```
                    ┌─────────────────────┐
                    │         Hub         │
                    │  ESP32 + web server │
                    │  hosts WiFi + UI     │
                    └──────────┬──────────┘
                               │ wireless
              ┌────────────────┼────────────────┐
              │                │                │
        ┌─────▼─────┐    ┌─────▼─────┐    ┌─────▼─────┐
        │  Pump      │    │  Pump      │    │  Pump      │
        │  module 1  │    │  module 2  │    │  module N  │
        └────────────┘    └────────────┘    └────────────┘
```

## Hub

- Hardware: ESP32 (WROOM or similar dev board), always powered (plugged in).
- Runs in **AP+STA mode simultaneously**: hosts its own AP (so the local web
  UI is always reachable even if home WiFi is down) while also joining the
  home WiFi as a station, purely to get an upstream path to the internet for
  the remote-access relay (below). Local UI doesn't depend on the home
  network; remote access does.
- Local web server (ESPAsyncWebServer is the usual choice) serving:
  - Status page: which satellites are online/detected, last-seen time,
    battery level, reservoir level / days-until-empty per satellite.
  - Schedule page: view/edit per-satellite watering schedule, rename
    satellites, global on/off.
  - Simple REST-ish endpoints so satellites can report in and pull their
    schedule.
- Persists schedule, satellite registry (names, reservoir capacities), and
  watering log to flash (e.g. LittleFS/SPIFFS + JSON, or NVS) so a power
  cycle doesn't lose state.

## Pump modules ("satellites")

- Hardware per module: ESP32 (or a cheaper ESP32-C3 if cost/size matters) +
  MOSFET or relay driving a small submersible/peristaltic pump + battery.
- Most modules will be battery powered and placed wherever the plant is, not
  next to an outlet — this drives most of the design constraints below.
- Each satellite is identified by a stable ID (its MAC address) and given a
  human-friendly name in the hub UI once it's fixed to a specific plant
  (e.g. "Fern — living room"). Renaming is purely a hub-side registry edit;
  the satellite itself doesn't need to know its own name.
- Behavior (Phase 1): wake up on a timer, check the float switch (below)
  before doing anything else, run the pump for a scheduled duration if it's
  due *and* the reservoir isn't flagged empty, check in with the hub with
  status (ran / skipped-empty / reservoir level, if using volume tracking),
  go back to deep sleep.
- Behavior (Phase 2 addition): also read humidity/temperature sensor(s) before
  deciding how much to water, and report readings to the hub for the status
  page.

## Dry-run protection

Cheap submersible pumps aren't rated to run dry — the motor relies on the
water for cooling/lubrication, so an empty reservoir can burn one out. Three
layers, each solving a different part of the problem:

1. **Float switch (safety-critical, hardware)** — a physical float switch at
   the "near-empty" level in the reservoir, wired straight into a GPIO. The
   satellite checks it *before* commanding the pump on, every wake cycle,
   independent of the hub or any network round-trip. This is the actual
   safety net — it works even if the hub is down, the WiFi is down, or the
   volume-tracking math below is wrong.
2. **Current sensing (diagnostic)** — the INA219 already planned for "pump
   detected" (see [hardware.md](hardware.md)) doubles as a dry-run check: a
   submersible pump draws a different current signature dry vs. primed with
   water. Lets the satellite catch subtler faults the float switch can't
   (e.g. clogged tubing keeping the float from ever reporting empty, or a
   partially-primed pump). Signature needs validating empirically once we
   have a real pump to test.
3. **Volume tracking (convenience, software)** — see below. This is a "fuel
   gauge" for planning ahead, not a safety mechanism — it's only as accurate
   as the pump's nominal flow rate, so it shouldn't be the only thing
   standing between the pump and running dry.

## Reservoir tracking & logging

- Each satellite has a configured **reservoir capacity** (e.g. 8000 mL),
  editable in the hub UI, alongside its name.
- The hub keeps a running **cumulative volume dispensed since last refill**
  per satellite: each watering event adds `duration_run × pump's nominal
  flow rate (mL/s at 5V)` to the total. This is an estimate (no flow sensor
  in the current plan), not a precise measurement — flags for the "days
  left" number to be treated as an approximation.
- A **"refill" action** in the hub UI resets a satellite's cumulative
  dispensed total to zero (user presses it after physically topping up the
  container).
- **Days-until-empty** is computed by projecting the *upcoming* schedule
  forward against remaining volume (`(capacity - dispensed) / scheduled
  mL-per-day`), rather than only averaging past usage — this gives a
  sensible number immediately after a refill, before any history exists.
  Historical actual-vs-expected volume (once current sensing or a future
  flow sensor gives real data) can later be used to calibrate the nominal
  flow rate per pump, improving the estimate over time.
- The hub logs every watering event (satellite, timestamp, duration,
  estimated volume, ran/skipped-empty outcome) to flash. Given flash size
  is limited, plan to keep fine-grained events for a rolling window (e.g.
  last 30 days) and collapse older entries into daily totals — full-detail
  history beyond that lives in the remote relay's storage instead (below),
  if remote access is in place.

## Remote access

Decision: **outbound relay**, not direct port-forwarding. The hub makes an
outbound connection to a small always-on relay rather than accepting inbound
connections from the internet — this avoids opening a port on the home
router (no direct internet exposure of the hub itself) and works regardless
of ISP CGNAT.

- Hub connects out (over its home-WiFi STA link) to a small relay — most
  likely **MQTT over TLS** to either a self-hosted broker on a cheap VPS
  (~$5/mo, full control, e.g. Mosquitto) or a managed free-tier broker (e.g.
  HiveMQ Cloud) to start with zero infrastructure to maintain.
- Hub **publishes**: satellite statuses, watering log events, reservoir
  levels/empty flags. Hub **subscribes**: schedule changes, on/off/refill
  commands issued remotely.
- A small always-on companion service on the relay side (if self-hosting)
  subscribes to the hub's topics and appends events to a local database
  (SQLite is plenty at this scale) — this is what gives durable history
  beyond the hub's own flash storage, and is what a remote dashboard
  (a simple web page, could even be an MQTT-over-websockets client talking
  to the broker directly for live state) reads from.
- Security: unique credentials (or client cert) per hub on the broker, TLS
  throughout, and the command topic requires the same authenticated
  connection — so nobody but the household can issue watering commands.
- A natural extension once this exists: push a notification (e.g. via
  ntfy.sh, Pushover, or a transactional email) when a reservoir goes empty,
  rather than requiring an active check of the dashboard while away.
- This is a second software component beyond the hub/satellite firmware —
  worth its own directory in the repo once we get here (not yet).

## Communication: hub ↔ modules

This is the biggest open decision. Options, given the hub needs to host its
own WiFi + web UI and modules are likely battery powered:

| Option | Pros | Cons |
|---|---|---|
| **WiFi station + deep sleep** (module joins hub's AP like a normal WiFi client, does an HTTP request, sleeps) | Simple, reuses the hub's existing web server/HTTP stack, easy to debug (it's just HTTP) | WiFi join + handshake costs real power/time even with deep sleep; range inside a house with walls may be marginal for far rooms |
| **ESP-NOW** (connectionless, low-power peer-to-peer WiFi protocol) | Very low power, fast wake-send-sleep cycle (~ms), no association overhead, good range for line-of-sight | No native multi-hop/relay (would need to hand-roll if range is an issue); hub needs a small bridge layer between ESP-NOW and the web UI |
| **MQTT over WiFi** (modules publish to a broker, e.g. running on the hub) | Clean pub/sub model, easy to extend, good tooling | Same WiFi power cost as option 1, plus a broker to run on the hub |

Leaning towards **ESP-NOW** for the module→hub link given battery life matters
more than protocol elegance, with the hub bridging ESP-NOW messages into its
own web UI/state. Worth prototyping both ESP-NOW and plain WiFi+deep-sleep
power draw before committing, though.

## Open questions

- [x] Hub network mode: resolved as **AP+STA simultaneously** — own AP for a
      home-WiFi-independent local UI, plus a home-WiFi station link purely
      to reach the internet relay. See Remote access section.
- [x] Module power source: outlet-powered where possible, solar+LiPo where
      not — see [hardware.md](hardware.md), both variants regulated to a
      common 5V rail so the rest of the circuit is power-source-agnostic.
- [x] What does "pump detected" mean exactly — draft answer in
      [hardware.md](hardware.md): an inline INA219 current sensor, so the
      satellite can tell the hub "commanded on but drew ~0mA" vs. "drew
      expected current," not just a heartbeat. Still open whether this is
      in scope for v1 or deferred (see hardware.md's open items).
- [ ] Water source per module: individual small reservoir per module (needs
      refilling before vacation) vs. tubing from a central reservoir/tank?
      Reservoir tracking section above assumes one reservoir per satellite —
      revisit if a shared tank is preferred instead.
- [ ] Range: how far are the farthest plants from where the hub would live?
      Determines whether ESP-NOW range is sufficient or a relay module is
      needed.
- [x] Remote access while on vacation: resolved as an outbound relay
      (MQTT-over-TLS to a small VPS or managed broker) — see Remote access
      section. Open sub-decision: self-hosted VPS vs. managed free-tier
      broker to start.

## Phase 2 (later): sensing

- Add humidity + temperature sensors per module (e.g. capacitive soil
  moisture sensor + a cheap temp/humidity combo like DHT22 or SHT31).
- Hub schedule logic becomes conditional: skip/shorten watering if soil
  moisture is already high, extend on hot/dry days.
- No architecture changes needed for this beyond adding sensor reads to the
  module's wake cycle and reporting the values — the wireless link and hub
  schedule model already support it.
