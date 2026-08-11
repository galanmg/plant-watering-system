# Hardware draft

Status (2026-08-04): hub built and running (ESP32-WROOM-32U). First pump
satellite built and proven end-to-end — relay-driven pump control confirmed
moving water, ESP-NOW check-in to the hub confirmed working. See
`firmware/` for what's actually flashed, and `docs/architecture.md`'s
Status section for what's still a placeholder vs. real. Part numbers below
are largely confirmed-in-use now rather than starting points, but treat
anything not marked "in use" as still a draft — swap for whatever's
actually in stock.

## Voltage strategy: standardize on 5V

The whole system — hub, satellites, pumps — runs on a single 5V rail:

- Cheap mini submersible pumps commonly come in a 5V-rated spec, so we don't
  need a separate voltage domain just for the pump.
- ESP32 dev boards natively accept 5V on their `5V`/`VIN` pin and regulate
  down to 3.3V on-board, so no separate 3.3V supply is needed anywhere.
- USB wall adapters (5V) are cheap, standard, and reusable across hub and
  outlet-powered satellites.
- For the battery/solar satellites, a small boost converter steps the
  LiPo's ~3.0–4.2V up to a clean 5V — so the MCU + driver + pump circuitry
  is *identical* regardless of whether a satellite is outlet- or
  solar-powered. Only the power stage feeding that 5V rail differs.

(12V pumps exist and push more flow/head, but for drip-watering a houseplant
that's overkill and would force a second regulated rail just for MCU logic.
Not worth the complexity at this scale.)

## Hub

Always-on, plugged into an outlet, lives somewhere central for WiFi/ESP-NOW
range.

| Part | Notes |
|---|---|
| ESP32-WROOM-32 dev board | Regular ESP32, not the -C3 mini — hub needs to run AP + web server + (likely) ESP-NOW bridging simultaneously, so a bit more headroom is nice. Any common dev board (e.g. "ESP32 DevKitC" style) works. |
| 5V/1A USB wall adapter + cable | Powers the board continuously via USB or the 5V pin. |
| 10-LED WS2812B (NeoPixel) strip/bar | Status panel — see [architecture.md](architecture.md)'s Status LEDs section. One GPIO data line + 5V + GND; color (green=pump, blue=monitor) and blink state are set entirely in firmware per LED, so the panel doesn't need rewiring if the pump/monitor mix changes. Driven from the same 5V rail as the rest of the hub. |
| Momentary push button (+ pull-down/pull-up as wired) | The status button — press to light the LED panel for a few seconds. One GPIO, standard software debounce. |
| Small enclosure | Vented plastic project box; keep the antenna area clear of metal, with the LED strip and button visible/accessible on the front face. |

**If range testing shows the hub can't reach far satellites:** swap for an
ESP32-WROOM-32U (u.FL connector) + external 2.4GHz antenna. Flagging now,
not buying yet — no point until we know we need it.

## Pump satellite — common parts

Same "brains" board for both the outlet-powered and solar/battery variants;
only the power stage (below) differs.

| Part | Notes |
|---|---|
| ESP32 dev board | **In use for the test build: ESP32-WROOM-32 (ELEGOO 3-pack, already purchased)** — fully capable (WiFi/ESP-NOW, deep sleep, GPIO/I2C), just bigger/pricier/thirstier in deep sleep than the ESP32-C3 mini originally planned here. Fine for outlet-powered satellites, which is the whole test build. Revisit C3 specifically if/when a solar/battery satellite gets built, where deep-sleep current actually matters. |
| Small 5V submersible pump | The common "3–6V mini submersible pump" hobby part, run at 5V. Low flow, which is actually right for drip-watering a single plant. **In use: RUNCCI-YUN mini submersible, DC 3–5V, 0.18A draw, brushless** (already purchased, 3-pack) — confirmed working in the first satellite build. |
| Switch: relay module (in use) or MOSFET (originally planned) | **Test build uses a GTIWUNG 5V 1-channel relay module** (opto-isolated, screw terminals COM/NO/NC, `DC+`/`DC-`/`IN` on the logic side) instead of a bare MOSFET — already on hand, and confirmed working end-to-end (relay clicks, pump moves water) as of the first satellite build. Trigger is **active-HIGH** on this specific module (GPIO HIGH energizes the relay) — note this is empirically confirmed per-module, not guaranteed for a different relay board; re-check if swapping module brands. Wiring: pump `+` through `NO`, relay `COM` to the 5V supply, pump `−` and relay `DC-` share ground with the ESP32. A bare logic-level MOSFET (AO3400/IRLZ44N class) remains a valid lower-cost/smaller alternative for later satellites if desired — not required, the relay works fine at this scale. |
| Flyback diode (1N5819 Schottky or 1N4148) | Across the pump terminals, to absorb the inductive kick when the switch turns off. **Not installed in the first satellite build** — relay contacts tolerate this small pump's inductive kick (180mA) better than a bare MOSFET would, so it wasn't a blocker to get the first working build done, but it's still good practice and cheap to retrofit if one turns up. |
| INA219 current/voltage sensor (I2C) | Inline with the pump's power feed. Lets the satellite tell the hub "commanded on, but drew ~0mA" (pump unplugged/dead) vs. "drew expected current" (working). This is the concrete answer to the open "what does pump detected mean" question — current sensing, not just a heartbeat. Also a secondary diagnostic for faults the reservoir monitor's float switch can't catch (e.g. clogged tubing). **Not yet installed** — the first satellite build proved relay+pump control only; dry-run current sensing is still open, see firmware TODOs. |
| Reserved I2C header pads | Not populated yet — for the Phase 2 humidity/temperature sensor (e.g. SHT31). Leaving the header now avoids a board respin later. |

### Pump satellite wiring (relay + pump)

Confirmed working as of the first satellite build. Two separate power
domains — the ESP32's own USB/logic supply, and the pump's 5V feed —
tied together only by a shared ground, never by a shared supply line:

```
SIGNAL SIDE — low voltage, from the ESP32 satellite board
────────────────────────────────────────────────────────────
  ESP32 GPIO26 ────────────────────────► Relay IN
  ESP32 GND    ────────────────────────► Relay DC- ──┐
                                                       │  shared
POWER SIDE — 5V feed (e.g. a cut phone-charger cable) │  ground
────────────────────────────────────────────────────  │  node
  5V supply  +  (red)   ──┬──────────►  Relay DC+     │
                          └──────────►  Relay COM     │
  5V supply  GND (black) ───────────────────────────────┘
                          │
                          └──────────►  Pump  −

  Relay NO ─────────────────────────────────────────► Pump  +
  Relay NC ─────────────────────────────────────────► (unused)
```

Trigger polarity is **active-HIGH** on the specific relay module in use
(GTIWUNG 5V, opto-isolated) — GPIO `HIGH` energizes the relay and
connects `COM`↔`NO`, powering the pump. `LOW` (the idle/boot default) is
fail-safe: pump disconnected. This was empirically confirmed, not
assumed — re-verify if a different relay module brand is ever swapped in
(see the table above).

Note: no float switch on the pump satellite itself — reservoirs are shared
across satellites, so level sensing lives on a separate **monitor
satellite** instead (below), one per physical container.

## Pump satellite — outlet-powered variant

For satellites near an outlet (no battery hassle needed).

| Part | Notes |
|---|---|
| 5V/1A USB wall adapter | Feeds the 5V rail directly — same rail powers both the ESP32-C3 and the pump (switched via the MOSFET). |

## Pump satellite — solar/battery variant

For satellites out of outlet reach.

| Part | Notes |
|---|---|
| 3.7V LiPo battery, 2000–3000mAh | Exact capacity to be sized once we've measured actual average draw (deep-sleep ESP32 + occasional short pump run is very low average current — likely overkill even at 2000mAh, but cheap insurance for a run of cloudy days). |
| Small 6V solar panel (1–2W) | Trickle-charges the LiPo. |
| Solar-capable LiPo charge controller (e.g. board built around MCP73871 or BQ24074) | Important: **not** a plain TP4056 — those expect a regulated 5V USB input, not a variable-voltage solar panel, and won't charge reliably or safely off solar without that regulation. |
| Small boost converter module (LiPo → 5V) | Produces the same clean 5V rail as the outlet variant, so the rest of the satellite circuit (MCU, MOSFET, pump) doesn't need to know or care which power variant it's on. |

## Monitor satellite — common parts

One per physical container, regardless of how many pump satellites draw
from it. No pump/MOSFET/current-sensing — it senses and reports, nothing
else. Uses the same outlet or solar/battery power-stage options as the pump
satellite (above), whichever fits where the container lives.

| Part | Notes |
|---|---|
| ESP32 dev board | Same board family as the pump satellite (see note there) — ESP32-WROOM-32 for the test build, for one firmware/tooling setup across both node types. |
| Mini float switch (vertical or horizontal, normally-open) | Mounted at the "near-empty" level inside the reservoir, wired to a GPIO (with pull-up/pull-down as appropriate for the switch type). This is the primary, hardware-only dry-run safety cutoff. Mechanical mounting (bracket, enclosure) is a DIY/3D-print detail per container shape — not a fixed part spec. |

## Open items to settle before ordering

- [ ] Confirm ESP32-C3 dev board model (some cheap ones lack a battery-charge
      circuit onboard — fine here since charging is handled by the dedicated
      solar charger board above, but worth double-checking pinout for deep
      sleep wake sources).
- [ ] Confirm pump flow rate is workable with intended tubing length/drip
      placement — may want to test one pump before ordering N of them.
- [ ] Decide whether solar variant is needed for the first build at all, or
      whether every current plant is close enough to an outlet for v1
      (simpler build; add solar for whichever plant needs it once it's
      identified).
- [ ] INA219 modules add ~$2–3 and I2C wiring per satellite — confirm this
      level of pump-fault detection is wanted for v1, vs. deferring to a
      simpler heartbeat-only "detected" status first and adding current
      sensing later.
- [ ] Float switch mounting depends on container shape/size (e.g. an 8L
      jerrycan vs. a wide bucket need different switch geometry/orientation)
      — pick per-container once actual containers are chosen, not a single
      universal part.
