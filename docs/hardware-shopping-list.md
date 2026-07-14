# Hardware — Shopping List, Purchase Status & Part Requirements

> Companion to [project-overview.md](project-overview.md).
> Scope: **1 hub + 2 pump modules**, scalable later by buying more module kits.
> Rule: before ordering anything, paste the listing/datasheet into a chat to proof-check.

- **Author:** Andrés Moreno (galanmg)
- **Last updated:** 2026-07-14

---

## 1. Long-lead order — status

| Item | Qty | Status | Validated listing / notes |
|------|-----|--------|---------------------------|
| Hub board: ESP32-WROOM-**32U** set (board + 3 dBi antenna + U.FL-to-SMA cable) | 1 | ✅ validated | DUBEUYEW set, Amazon.es ASIN **B0DLVWZBL5**. Inspect pins on arrival (a review reported bent pins). Attach antenna once, before first power-up, then leave connected |
| Satellite boards: ESP32 DevKit, PCB antenna | 3-pack | ✅ validated | ELEGOO 3× ESP-32S, Amazon.es ASIN **B0D8T5XD3P** — WROOM-32, CP2102, USB-C, 30-pin. 2 in use + 1 spare. ⚠️ picky with USB-C-to-C cables: keep a USB-A-to-C data cable for flashing. (AZDelivery 3-pack B0D8WBG557 was validated first but went out of stock) |
| Contingency: 1 extra 32U set | 1 | optional | Same as hub. Swap into any satellite location that fails the range test |
| Pumps: **5V mini submersible**, 3-pack + 3 m PVC tubing | 1 pack | ✅ validated | RUNCCI-YUN 3×, Amazon.es ASIN **B082PM8L6X** — DC 3–5V brushless, 100–200 mA, ~100 L/h, tubing 5.54 mm ID / 8.2 mm OD included. 2 in use + 1 spare. See caveats in §3 |
| Logic-level MOSFET driver module | 3 (2 + 1 spare) | 🔍 pending | ⚠️ MUST switch fully on with a **3.3V** gate signal. Plain IRF520 modules often need 5V — verify before buying. Search "MOSFET trigger switch driver module" |
| Breadboards | 1 kit (2× 830 + 2× 400 pts) | ✅ validated | BOJACK kit, Amazon.es ASIN **B0B18G3V5T** — includes 126 M-M jumper wires. Splice two boards side-by-side for comfortable ESP32 access |
| Dupont wires (M-M / M-F / F-F) | 1 kit (120 pcs) | ✅ recommended | ELEGOO 120pc — needed because BOJACK's included wires are M-M only and MOSFET modules use male header pins |

Note (2026-07-14): pump decision reverted from peristaltic to submersible —
true 5V peristaltics barely exist; 12V ones would break the all-USB power
design and cost €15–20/unit. Peristaltic stays as the per-module upgrade
path. See decision log #4 in [project-overview.md](project-overview.md).

## 2. Buy later, locally / Amazon (fast shipping)

| Item | Qty (approx) | Notes |
|------|--------------|-------|
| USB chargers 5V ≥2A | 1 per device (3) | Pumps draw only 100–200 mA — any decent charger works |
| USB-A-to-C data cables | 1–2 | For flashing (C-to-C cables often fail with these boards) |
| Lever/twist wire connectors (Wago-style) | small pack | Pump cables are short — extend them to reach the MOSFET module without soldering |
| Barbed T/Y splitters (~5.5 mm) | a few | For the multi-pot indoor module; match tubing ID |
| Water reservoirs | 2 | **Oversized** — the main dry-run mitigation. Remember the pump body must stay submerged, so the last few cm are dead volume |
| Project enclosures | 3 | ⚠️ **PLASTIC**, never metal (blocks 2.4 GHz). Electronics mounted above water line |
| Screw-terminal breakout boards, **30-pin** ESP32 | 2 | For final installs after breadboard stage (ELEGOO boards are 30-pin) |

## 3. Pump caveats (RUNCCI-YUN submersibles)

- **Never run dry** — the motor needs water for cooling; dry running kills it.
  System-level mitigations: oversized reservoirs + conservative volume margin
  on the hub. Pumps are ~€4 each: treat as sacrificial, keep the spare.
- Nominal rating is 3–4.5V; at 5V it is slightly overdriven. Works (reviewers
  run them off USB), just don't expect maximum lifespan.
- "300 h continuous life" sounds short but our duty cycle is seconds per day —
  that's years of watering bursts.
- Flow varies with water level → calibrate mL/s at typical fill level, and
  keep the dosing margin generous.
- Cables are short and bare-ended: bare ends screw straight into MOSFET
  terminals (good), but plan wire extensions (lever connectors).
- No intake filter — keep reservoirs covered so debris/hair doesn't clog them.

## 4. Validation checklist per remaining part

- **MOSFET module (the pending one):** triggers fully ON with 3.3V gate
  (logic-level part, e.g. IRLZ44N-class); load side handles ≥5V and ≥0.5 A
  with margin; screw terminals preferred. Plain IRF520 boards usually need
  5V gate — reject unless proven 3.3V-capable.
- **Any substitute board:** WROOM-32 module, CP2102 or CH340 serial chip,
  USB port for flashing, breadboard-friendly.

## 5. Wiring rules (safety notes)

- Pump power comes **from the charger's 5V rail, through the MOSFET module** —
  never from the ESP32's 5V pin.
- Common ground between ESP32, MOSFET module and pump supply.
- On the 32U hub: antenna attached before first power-up, then left alone
  (U.FL connectors tolerate few mating cycles; an open RF connector reflects
  transmit power back into the amplifier).
- Keep water, tubing and reservoirs physically below/away from the
  electronics (drips fall down).
