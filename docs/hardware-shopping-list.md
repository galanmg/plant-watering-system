# Hardware — Shopping List & Part Requirements

> Companion to [project-overview.md](project-overview.md).
> Scope: **1 hub + 2 pump modules**, scalable later by buying more module kits.
> Before ordering anything: paste the listing/datasheet into a chat to proof-check.

- **Author:** Andrés Moreno (galanmg)
- **Last updated:** 2026-07-14

---

## 1. Order NOW (long lead time, e.g. AliExpress)

| Item | Qty | What to search for | Reference / spec to match |
|------|-----|--------------------|---------------------------|
| ESP32 board, **external antenna** (hub) | 1 | "ESP32 WROOM-32U external antenna" / "ESP32 DevKit U.FL" | WROOM-**32U** variant, U.FL/IPEX connector |
| 2.4 GHz antenna for the hub | 1 (2 if not bundled with board) | "2.4GHz WiFi antenna U.FL IPEX 3dBi" | 2.4 GHz, U.FL/IPEX connector, ~3 dBi |
| ESP32 board, PCB antenna (modules + spares) | 4 | "ESP32 DevKit V1 30 pin" / "ESP32 WROOM-32 development board" | Standard WROOM-32 DevKit. Avoid ESP32-C3 "SuperMini" (weak antenna through walls) |
| Contingency: 1 extra 32U + antenna | 1 + 1 | (same as hub) | Swap into any module location that fails the range test |
| 5V peristaltic pump | 3 (2 + 1 spare) | "peristaltic pump 5V dosing" | Match specs of Adafruit #3910 (5–6V, self-priming): https://www.adafruit.com/product/3910 |
| Logic-level MOSFET driver module | 3 (2 + 1 spare) | "MOSFET trigger switch driver module" | ⚠️ MUST switch with a **3.3V** gate signal (logic-level). Verify before buying |
| Silicone tubing (spare, consumable) | 1 roll | "silicone tubing 3mm ID" | Match inner diameter to the pump's barbs (usually ~3 mm ID) |

**Total boards: 6** (1 hub-32U + 4 standard + 1 contingency-32U). The 2 standard
spares also serve the future car project.

## 2. Buy LATER, locally / Amazon (fast shipping)

| Item | Qty (approx) | Notes |
|------|--------------|-------|
| USB chargers 5V ≥2A | 1 per device (3) | Must supply pump peak current with margin |
| USB cables (data-capable for flashing) | 3+ | Some charge-only cables cannot flash boards |
| Breadboards | 2 | Prototyping before soldering anything |
| Jumper wires (M-M, M-F) | 1 kit | |
| Barbed T/Y splitters | a few | For the multi-pot indoor module |
| Water reservoirs | 2 | Any food container works to start |
| Project enclosures | 3 | ⚠️ **PLASTIC**, never metal (blocks 2.4 GHz) |

## 3. Validation checklist per part (before paying)

Paste the listing into a chat and confirm:

- **ESP32 boards:** WROOM-32 (or 32U for hub), USB port for flashing,
  breadboard-friendly pin spacing.
- **Pump:** runs at 5V; note stall/max current (must be < charger capacity with
  margin); barb diameter noted (to buy matching tubing).
- **MOSFET module:** triggers fully ON with 3.3V gate signal (logic-level part,
  e.g. IRLZ44N-class). Plain IRF520 modules often need 5V gate — check carefully.
- **Antenna:** 2.4 GHz (NOT 868 MHz / LoRa / Sigfox), U.FL/IPEX connector.

## 4. Wiring rules (safety notes to remember)

- Pump power comes **from the charger's 5V rail, through the MOSFET module** —
  never from the ESP32's 5V pin (limit ~500 mA, pumps can exceed it).
- Common ground between ESP32, MOSFET module and pump supply.
- On the 32U hub: **connect the antenna before powering** the board.
- Keep water, tubing and reservoirs physically below/away from the electronics
  (drips fall down).
