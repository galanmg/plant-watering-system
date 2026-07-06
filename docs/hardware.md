# Hardware draft

Status: draft, nothing ordered yet. Part numbers below are starting points
for sourcing, not a locked BOM — treat as "this class of part," swap for
whatever's actually in stock when we order.

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
| Small enclosure | Vented plastic project box; keep the antenna area clear of metal. |

**If range testing shows the hub can't reach far satellites:** swap for an
ESP32-WROOM-32U (u.FL connector) + external 2.4GHz antenna. Flagging now,
not buying yet — no point until we know we need it.

## Satellite (pump module) — common parts

Same "brains" board for both the outlet-powered and solar/battery variants;
only the power stage (below) differs.

| Part | Notes |
|---|---|
| ESP32-C3 mini dev board | Cheaper and smaller than full ESP32, still supports WiFi/ESP-NOW and deep sleep. Good fit since satellites just wake, report, maybe pump, sleep. |
| Small 5V submersible pump | The common "3–6V mini submersible pump" hobby part, run at 5V. Low flow, which is actually right for drip-watering a single plant. |
| Logic-level N-channel MOSFET (e.g. AO3400 or IRLZ44N) | Low-side switch between pump's negative terminal and ground. Gate driven from a GPIO through a ~100Ω series resistor; add a 10kΩ pull-down gate-to-ground so the pump can't twitch on during MCU boot/reset. |
| Flyback diode (1N5819 Schottky or 1N4148) | Across the pump terminals, to absorb the inductive kick when the MOSFET switches off. Motors are inductive loads — skip this and you'll eventually kill a MOSFET. |
| INA219 current/voltage sensor (I2C) | Inline with the pump's power feed. Lets the satellite tell the hub "commanded on, but drew ~0mA" (pump unplugged/dead/reservoir dry if using a pump that stalls dry) vs. "drew expected current" (working). This is the concrete answer to the open "what does pump detected mean" question — current sensing, not just a heartbeat. Doubles as a secondary dry-run diagnostic alongside the float switch below. |
| Mini float switch (vertical or horizontal, normally-open) | Mounted at the "near-empty" level inside the reservoir, wired to a GPIO (with pull-up/pull-down as appropriate for the switch type). This is the primary, hardware-only dry-run safety cutoff — checked every wake cycle before the pump is ever commanded on, independent of the hub, network, or the volume-tracking estimate. |
| Reserved I2C header pads | Not populated yet — for the Phase 2 humidity/temperature sensor (e.g. SHT31). Leaving the header now avoids a board respin later. |

## Satellite — outlet-powered variant

For satellites near an outlet (no battery hassle needed).

| Part | Notes |
|---|---|
| 5V/1A USB wall adapter | Feeds the 5V rail directly — same rail powers both the ESP32-C3 and the pump (switched via the MOSFET). |

## Satellite — solar/battery variant

For satellites out of outlet reach.

| Part | Notes |
|---|---|
| 3.7V LiPo battery, 2000–3000mAh | Exact capacity to be sized once we've measured actual average draw (deep-sleep ESP32 + occasional short pump run is very low average current — likely overkill even at 2000mAh, but cheap insurance for a run of cloudy days). |
| Small 6V solar panel (1–2W) | Trickle-charges the LiPo. |
| Solar-capable LiPo charge controller (e.g. board built around MCP73871 or BQ24074) | Important: **not** a plain TP4056 — those expect a regulated 5V USB input, not a variable-voltage solar panel, and won't charge reliably or safely off solar without that regulation. |
| Small boost converter module (LiPo → 5V) | Produces the same clean 5V rail as the outlet variant, so the rest of the satellite circuit (MCU, MOSFET, pump) doesn't need to know or care which power variant it's on. |

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
