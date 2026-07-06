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
- Runs a WiFi access point (or joins home WiFi — see open question below) and
  a lightweight web server (ESPAsyncWebServer is the usual choice) serving:
  - Status page: which modules are online/detected, last-seen time, battery
    level per module (once modules report it).
  - Schedule page: view/edit per-module watering schedule, global on/off.
  - Simple REST-ish endpoints so the modules can report in and pull their
    schedule.
- Persists schedule + last-known state to flash (e.g. LittleFS/SPIFFS + JSON,
  or NVS) so a power cycle doesn't lose the schedule.

## Pump modules

- Hardware per module: ESP32 (or a cheaper ESP32-C3 if cost/size matters) +
  MOSFET or relay driving a small submersible/peristaltic pump + battery.
- Most modules will be battery powered and placed wherever the plant is, not
  next to an outlet — this drives most of the design constraints below.
- Behavior (Phase 1): wake up on a timer, check in with the hub, run the pump
  for a scheduled duration if it's due, report status, go back to deep sleep.
- Behavior (Phase 2 addition): also read humidity/temperature sensor(s) before
  deciding how much to water, and report readings to the hub for the status
  page.

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

- [ ] Hub network mode: does the hub run as its own AP (phone/laptop connects
      directly to it to see the status page), or join the home WiFi and be
      reachable at a normal LAN address? AP mode is simpler and works even if
      home WiFi is down; joining home WiFi means you can check status from
      anywhere the phone already has WiFi/is on the same network, and enables
      remote-over-internet access later if wanted.
- [ ] Module power source: battery only (what capacity, what recharge/replace
      cadence), or small solar top-up for outdoor plants?
- [ ] What does "pump detected" mean exactly — module presence/heartbeat, or
      actually sensing that the pump moved water (current sense on the motor,
      flow sensor, water level in reservoir)? Affects whether we need current
      sensing hardware per module.
- [ ] Water source per module: individual small reservoir per module (needs
      refilling before vacation) vs. tubing from a central reservoir/tank?
- [ ] Range: how far are the farthest plants from where the hub would live?
      Determines whether ESP-NOW range is sufficient or a relay module is
      needed.
- [ ] Remote access while on vacation: is "connect to the hub's WiFi to check
      status" enough, or do we want visibility/control from actual internet
      (would need the hub on home WiFi + port forwarding, or a cloud relay)?

## Phase 2 (later): sensing

- Add humidity + temperature sensors per module (e.g. capacitive soil
  moisture sensor + a cheap temp/humidity combo like DHT22 or SHT31).
- Hub schedule logic becomes conditional: skip/shorten watering if soil
  moisture is already high, extend on hot/dry days.
- No architecture changes needed for this beyond adding sensor reads to the
  module's wake cycle and reporting the values — the wireless link and hub
  schedule model already support it.
