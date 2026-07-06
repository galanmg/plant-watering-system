# Hardware shopping list request

I'm building a DIY automated plant-watering system (ESP32-based). The
design is settled — what I need now is help turning each part *class*
below into an actual, currently-purchasable product (model/part number,
approximate price, a reasonable vendor like Amazon/AliExpress/DigiKey/
Mouser), while keeping everything consistent with the constraints noted.

Please recommend specific products for each line item, grouped the same
way as below, and flag anything where a cheaper/better alternative exists
that still meets the constraint.

## System constraints (apply to everything)

- **Single 5V rail throughout** — hub, satellites, and pumps all run on
  5V. Prefer parts that are natively 5V or regulate cleanly to 5V; avoid
  anything that forces a second voltage domain (e.g. skip 12V pumps).
- Two MCU families used across the system: a full **ESP32 (WROOM-32)** for
  the hub, and **ESP32-C3** mini boards for satellites (cheaper/smaller,
  still supports WiFi/ESP-NOW + deep sleep).
- This is a hobby/DIY build — favor cheap, commonly-stocked hobbyist parts
  over industrial/precision equivalents, unless a constraint below rules
  a cheap option out.

## Quantities

Ordering for an initial test build first:

- **1× hub**
- **1× reservoir + 1× monitor satellite**
- **3× pump satellites**, all drawing from that one reservoir

Planned eventual scale (not ordering yet, but worth knowing when picking
vendors/pack sizes): **3–4 reservoirs total, at least 3 pump satellites
each** — so roughly 9–12+ pump satellites and 3–4 monitor satellites down
the line. Prefer parts sold in multi-packs or with good per-unit pricing
at that kind of quantity where it doesn't compromise quality.

## Hub (qty: 1)

| Part | Requirement |
|---|---|
| MCU dev board | ESP32-WROOM-32 (regular, not -C3) — needs headroom to run a web server + WiFi station + ESP-NOW bridging simultaneously. Common "ESP32 DevKitC"-style board. |
| Power supply | 5V/1A USB wall adapter + cable, always plugged in. |
| Status LED strip | **10-LED WS2812B (NeoPixel) addressable strip or bar**, individually addressable (need per-LED color: green/blue, and per-LED blink control via firmware). |
| Status button | Momentary push button (normally-open), panel/PCB-mount style. |
| Enclosure | Small vented plastic project box, big enough to mount the board, LED strip, and button on/through the front face; keep the WiFi antenna area clear of metal. |

## Pump satellite (qty: 3 for now, plan for growth)

| Part | Requirement |
|---|---|
| MCU dev board | ESP32-C3 mini dev board, deep-sleep capable. |
| Pump | Small 5V-rated mini submersible pump (common "3–6V mini submersible pump" hobby part, run at 5V). Low flow is fine/preferred — this is drip-watering a single houseplant, not irrigation. |
| MOSFET | Logic-level N-channel MOSFET for a low-side switch (e.g. AO3400 or IRLZ44N class) — must be gate-driven directly from a 3.3V GPIO. |
| Flyback diode | Small Schottky or general-purpose diode (e.g. 1N5819 or 1N4148) to go across the pump terminals. |
| Current/voltage sensor | INA219 I2C current/voltage sensor breakout, inline with the pump's power feed. |
| Resistors | ~100Ω gate series resistor + ~10kΩ gate pull-down per unit (can just be a resistor assortment kit, not a dedicated purchase). |
| Power stage — outlet variant | 5V/1A USB wall adapter (if this satellite will sit near an outlet). |
| Power stage — solar/battery variant | 3.7V LiPo battery (2000–3000mAh) + small 6V 1–2W solar panel + a **solar-capable** LiPo charge controller (built around a chip like MCP73871 or BQ24074 — explicitly *not* a plain TP4056, which expects a regulated 5V USB input, not raw solar) + a small boost converter module (LiPo → clean 5V). |

For the test build, assume **outlet-powered** for all 3 pump satellites
unless you know of a solar/battery kit cheap enough to be worth building
both variants from the start — otherwise just quote the outlet variant for
now and the solar/battery parts separately as a "for later" list.

## Reservoir monitor satellite (qty: 1 for now, plan for growth)

| Part | Requirement |
|---|---|
| MCU dev board | ESP32-C3 mini dev board, same as pump satellites (shared firmware/tooling). |
| Float switch | Mini float switch (vertical or horizontal, normally-open), sized/oriented for mounting at the "near-empty" level inside the reservoir. Mechanical mounting/bracket is DIY/3D-printed on my end — just need the electrical switch component recommended here. |
| Power stage | Same outlet or solar/battery options as the pump satellite above — recommend the outlet variant for the test build. |

## What I don't need recommendations on

- The 3D-printed/DIY mechanical mounting for the float switch (per-container,
  handled separately).
- Software/firmware — this list is purely for physical parts to buy.
