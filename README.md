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

Building, not just designing anymore (2026-08-04): the hub is running
(WiFi, NTP-synced clock, a status web page), and the first pump satellite
is proven end-to-end — wirelessly checked in with the hub and physically
pumped water on command. See [docs/architecture.md](docs/architecture.md)'s
"Current implementation status" for exactly what's real vs. still a
placeholder, and [docs/firmware.md](docs/firmware.md) for how to build/flash.

## Roadmap

1. **Phase 1** — hub + pump modules, manual schedule, status page, dry-run
   protection, reservoir/usage tracking, remote access via an outbound relay.
   - [x] Hub: WiFi + NTP + status web page + ESP-NOW receiver
   - [x] First pump satellite: ESP-NOW check-in + relay-driven pump, proven
         moving water
   - [ ] Real hub↔satellite protocol (hub decides run/don't-run, satellite
         reports outcome) — current satellite firmware is a one-shot test
   - [ ] Deep sleep on satellites
   - [ ] Dry-run protection (INA219 current sensing)
   - [ ] Clone the proven satellite build onto the other 2 pump satellite
         boards
   - [ ] Monitor satellite (float switch)
   - [ ] Web-based schedule/status UI, satellite registry
   - [ ] Status LED panel
   - [ ] Remote access (outbound relay)
2. **Phase 2** — humidity/temperature sensing, adaptive watering.
