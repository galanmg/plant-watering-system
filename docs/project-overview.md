# Project Overview — Plant Watering System

> **Purpose of this document:** master reference for the project. Any future chat
> (Claude, Claude Code, or a human) should read this first to understand what has
> been decided, what stage we are in, and what comes next.
> **Keep it updated:** when a step is completed or a decision changes, edit this file.

- **Author:** Andrés Moreno (galanmg)
- **Repo:** https://github.com/galanmg/plant-watering-system
- **Deadline driver:** system must work before holidays in **early September 2026**
- **Last updated:** 2026-07-14

---

## 1. Goal

Automated watering system so house plants survive vacations without a neighbor.

- 1 **hub** (ESP32) hosting a WiFi network + small web app: module status
  (detected / not detected), view & edit watering schedule, global on/off.
- **Pump modules** (ESP32 + pump) around the house, wireless.
- **Phase 2 (winter project, NOT now):** humidity/temperature sensors per module
  for adaptive watering.

## 2. Scope for September (Phase 1)

- **1 hub + 2 pump modules** (one balcony, one indoors).
- The indoor module may water several pots from one pump using T-splitters
  (same dose to all pots at once — acceptable fallback).
- Design must **scale by repetition**: adding module #3, #4… later means
  flashing the same firmware on another identical module and registering it
  on the hub. No rework of existing parts.
- Dry-run protection and reservoir/usage tracking (from original roadmap).
- Phase 2 sensors are explicitly **out of scope** until winter.

## 3. Key decisions made (decision log)

| # | Decision | Choice | Why |
|---|----------|--------|-----|
| 1 | Microcontroller | ESP32 (WROOM-32 DevKit family) | Cheap, well documented, doubles as playground for a future car project |
| 2 | Hub ↔ module comms | ESP-NOW | ESP32-to-ESP32, no router needed, robust for tiny command packets |
| 3 | Power | Everything ~5V via standard USB chargers | Simple, safe, chargers everywhere |
| 4 | Pump type (v1) | **5V mini submersible** (was: peristaltic) | ~€4 vs €15–20/unit — true 5V peristaltics barely exist and 12V ones would force adapters + buck converters, breaking decision #3. Trade-off accepted: dry running damages the pump and dosing is time-based (flow varies with water level). Mitigations: oversized reservoirs, conservative volume-tracking margin, pumps cheap enough to be sacrificial. **Peristaltic remains the targeted per-module upgrade if the project proves itself** — swap pump + recalibrate, nothing else changes |
| 5 | Pump driver | Logic-level MOSFET driver module (3.3V-triggerable) | Cheaper than relay, silent, no wearing contacts, supports PWM. NOT a relay |
| 6 | Hub antenna | ESP32-WROOM-**32U** + external 2.4 GHz U.FL antenna | Central node: boosting it helps every link. ⚠️ Attach antenna once, before first power-up, then leave it connected (open connector reflects TX power into the amplifier; U.FL is rated for few mating cycles; 32U has no PCB-antenna fallback) |
| 7 | Module antennas | Standard PCB antenna | Good enough; contingency = 1 spare 32U to swap into a flaky location after real-world range testing |
| 8 | Pump power path | Pump fed from USB charger rail through the MOSFET | NEVER from the ESP32's 5V pin (~500 mA limit) — and it keeps motor noise off the board's supply |

Rule: **before buying any part, paste its datasheet/listing into a chat to
proof-check** voltage, current draw, 3.3V logic-level compatibility and tubing fit.

## 4. Steps to follow (the plan)

Mark items as done by changing `[ ]` to `[x]`.

### Stage A — Preparation (now)
- [x] Requirements written (README)
- [x] Architecture decisions (see decision log above)
- [ ] Order long-lead electronics → see [hardware.md](hardware.md) for
      per-item purchase status (boards/breadboards/pumps validated, MOSFET pending)
- [ ] While shipping: set up dev environment (Arduino IDE or PlatformIO + ESP32 support)
- [ ] While shipping: start hub web app + scheduling logic (can be developed with
      just one board, or none, since ESP-NOW modules can be simulated)

### Stage B — First hardware bring-up (parts arrived)
- [ ] Flash "hello world" / blink on each board to verify them
      (⚠️ if a board seems dead over USB-C, try a USB-A-to-C data cable first —
      ESP32 boards are picky with C-to-C cables)
- [ ] **Range test FIRST**: simple ESP-NOW ping sketch, walk boards to their real
      locations (balcony, indoor spot) BEFORE building anything into enclosures.
      If a module location is flaky → swap in the spare 32U + antenna
- [ ] Breadboard ONE pump module: ESP32 + MOSFET driver + pump, get reliable on/off
- [ ] Calibrate each pump: measure mL per second of runtime at the real reservoir
      level (needed for dosing; submersible flow varies with water level, so
      calibrate at typical fill and re-check near "low")

### Stage C — Core system
- [ ] Hub ↔ module ESP-NOW protocol (module announces itself, hub sends commands,
      module acknowledges)
- [ ] Hub web app: status page (module detected / not detected)
- [ ] Watering schedule: view, edit, global off
- [ ] Safety: dry-run protection (volume tracking + conservative margin +
      oversized reservoirs), reservoir/usage tracking, "refill" button

### Stage D — Deployment
- [ ] Replicate to second module (identical firmware)
- [ ] Plastic enclosures (⚠️ plastic, not metal — metal blocks 2.4 GHz),
      antenna end clear of metal/batteries/water; electronics above water line
- [ ] Tubing + T-splitters for multi-pot indoor module
- [ ] Full vacation simulation: run unattended for several days while home
- [ ] Remote access via outbound relay (from original roadmap)

### Stage E — Phase 2 (winter, out of scope now)
- [ ] Humidity/temperature sensors, adaptive watering
- [ ] Optional pump upgrade: peristaltic per module where precise dosing matters
- [ ] Optional INA219 current sensing ("pump detected" diagnostics)

## 5. Repo layout

```
plant-watering-system/
├── README.md
└── docs/
    ├── architecture.md      (system design & open questions)
    ├── project-overview.md  (this file — master plan & decision log)
    └── hardware.md          (shopping list, purchase status & part requirements)
```

Suggested future folders as code appears: `firmware/hub/`, `firmware/module/`.

## 6. Working conventions

- First project of the author → keep names, structure and code **simple and
  beginner-level**; avoid clever abstractions.
- Prefer identical firmware for all pump modules (differences via config, not code).
- Update the decision log when anything above changes.
