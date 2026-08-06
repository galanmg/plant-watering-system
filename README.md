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
2026-08-06: the hub is running (WiFi, NTP-synced clock with a fallback
clock for outages, a status/control web page), and the first pump
satellite runs the real check-in/command/report protocol end-to-end, with
a measured flow rate (12.5mL/s) driving a working manual "water N mL"
control. See docs/architecture.md's "Current implementation status" for
exactly what's real vs. still a placeholder, and
[docs/firmware.md](docs/firmware.md) for how to build/flash.

## Roadmap

1. **v1** — hub + 2 pump satellites, oversized reservoirs + volume-tracking
   as the dry-run safety net (see architecture.md's Dry-run protection).
   Monitor satellites, INA219 current sensing, and the LED panel are
   designed but explicitly deferred past v1.
   - [x] Hub: WiFi + NTP (with power-outage-resilient fallback clock) +
         status/control web page + ESP-NOW
   - [x] First pump satellite: real check-in → command → run → report
         protocol, relay-driven pump, proven moving water
   - [x] Flow-rate calibration (measured, hardcoded) + manual "water N mL"
         web control
   - [ ] Nightly schedule dosing a real amount (currently a 50mL
         placeholder) rather than a bench-test value
   - [ ] Clone the proven satellite build onto the second pump satellite
         board
   - [ ] Web-based schedule/status UI, satellite registry
   - [ ] Volume tracking + oversized-reservoir dry-run safety net
2. **Post-v1** — monitor satellite (float switch), INA219 dry-run current
   sensing, status LED panel, remote access (outbound relay).
3. **Phase 2 (winter)** — humidity/temperature sensing, adaptive watering.
