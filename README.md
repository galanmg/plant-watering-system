# Plant Watering System

An automated watering system for the house, built to survive vacations without
begging a neighbor to come water the plants.

## The problem

A couple of times a year we go on vacation and there's no one reliable to water
the plants. They wither.

## The idea

- A **hub** (ESP32) that:
  - Hosts a WiFi network and a small web app.
  - Shows the status of every pump module (detected / not detected).
  - Lets us view and edit the watering schedule, or turn watering off entirely.
- Several **pump modules** scattered around the house, each driving a small
  water pump, communicating wirelessly with the hub.
- **Later upgrade**: humidity/temperature sensors per module, so watering
  amount adapts to conditions (more on hot days, less on cold/humid ones).
  Secondary priority — core scheduling/on-off comes first.

## Status

v1 target: 1 hub + 2 pump satellites, working before early September 2026
(see [docs/architecture.md](docs/architecture.md)'s Overview). As of
2026-08-07: the hub runs a real live-updating web control panel — a
persisted, multi-satellite registry, per-slot (up to 5x/day) independent
scheduling with its own dose per slot, manual test/water controls, and a
confirmed command/report/ack protocol with retries. The first pump
satellite is fully working end to end; a real WiFi-drop scenario (router
vanishes, electricity never lost) was deliberately tested and confirmed a
scheduled watering still fires while the hub is unreachable. A genuine
multi-day soak test is now the open question — see
docs/architecture.md's "Current implementation status" for the full
writeup, and [docs/firmware.md](docs/firmware.md) for how to build/flash.

## Roadmap

1. **v1** — hub + 2 pump satellites, oversized reservoirs + volume-tracking
   as the dry-run safety net (see architecture.md's Dry-run protection).
   Monitor satellites, INA219 current sensing, and the LED panel are
   designed but explicitly deferred past v1.
   - [x] Hub: WiFi + NTP (with power-outage-resilient fallback clock) +
         live-updating status/control web page + ESP-NOW
   - [x] First pump satellite: real check-in → command → run → report →
         ack protocol (with retries + dedup), relay-driven pump, proven
         moving water
   - [x] Flow-rate calibration (measured, hardcoded) + manual "water N mL"
         web control
   - [x] Satellite registry persisted to flash, survives reboots
   - [x] Per-satellite, per-slot (up to 5x/day) schedule with independent
         dose per slot — replaces the old single nightly placeholder dose
   - [x] Multi-day reliability hardening: WiFi-drop detection + auto-
         recovery (tested live, including a scheduled watering firing
         mid-outage), periodic NTP resync, preventive daily reboot
   - [ ] Genuine multi-day soak test (day-rollover re-arming, heap over
         time — both reasoned through, neither actually observed yet)
   - [ ] Clone the proven satellite build onto the second pump satellite
         board
   - [ ] Volume tracking + oversized-reservoir dry-run safety net
2. **Post-v1** — monitor satellite (float switch), INA219 dry-run current
   sensing, status LED panel, remote access (outbound relay).
3. **Phase 2 (winter)** — humidity/temperature sensing, adaptive watering.
