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

Early design phase. See [docs/architecture.md](docs/architecture.md) for the
current thinking and open questions.

## Roadmap

1. **Phase 1** — hub + pump modules, manual schedule, status page, dry-run
   protection, reservoir/usage tracking, remote access via an outbound relay.
2. **Phase 2** — humidity/temperature sensing, adaptive watering.
