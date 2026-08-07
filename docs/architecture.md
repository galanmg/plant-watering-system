# Architecture

Status: hub + first pump satellite built and proven end-to-end, with a
real multi-satellite registry, per-slot scheduling, a confirmed
command/report/ack protocol, a live-updating web UI, and WiFi/power-outage
resilience. First physical steps toward the v1 scope below. This doc is
updated as decisions get made — treat "Open questions" as the running
to-do list. See [project-overview.md](project-overview.md) for the
decision log and step checklist, and [hardware.md](hardware.md) for the
shopping list and purchase status.

Last updated: 2026-08-07

## Current implementation status

What's actually running, as of the first satellite build (v1 target is 2
pump satellites — see Overview below; only 1 is built so far):

- **Hub** (`firmware/hub/`) — joins home WiFi, syncs time via NTP (Madrid
  TZ), serves a web page (`plant-hub.local`) that's become the real control
  surface for the system:
  - **Satellite registry, persisted to flash** (NVS via `Preferences`) —
    a satellite auto-registers into a free slot (of 4) the moment its
    first check-in arrives, no manual "add" step. Each gets a card of its
    own on the page: live status dot (green *En línea* / amber
    *Esperando confirmación* while a command is in flight and check-ins
    have naturally paused / grey *Sin conexión* if genuinely stale — red
    is reserved for a future fault alarm, unused so far), last-seen and
    last-watered times (humanized: seconds → minutes → hours → days), a
    rename control (MAC tucked into an "Ajustes" panel, not shown by
    default), manual "Regar ahora" (mL) and "Probar" (fixed 5s) buttons,
    and a "Historial reciente" table (newest-first, last 10 events,
    always visible even with zero entries).
  - **Per-slot scheduling, not one dose for the whole satellite** — each
    satellite has up to 3 independent time slots, each with its own
    enable flag, time-of-day, *and dose in mL*. This is deliberate: more
    water at night, a light midday top-up on hot days, spread across the
    day to avoid pooling, all on one pump. The collapsed "Editar horario"
    summary shows the count of active slots and the *summed* daily total
    so you don't have to expand it to sanity-check the day's dosing.
  - **Live status without full reloads** — the page polls a small
    `/status.json` endpoint every 5s and patches just the status
    dot/label/last-seen/last-watered/history-table text by element id.
    Deliberately never touches the schedule/rename forms, so it can't
    interrupt an in-progress edit the way a full-page auto-refresh would.
    The header card is itself a plain link back to `/` (a full reload) —
    easier to hit than the browser's own refresh button, especially on
    mobile; only applies its hover shadow under `(hover: hover)` so it
    doesn't trigger iOS Safari's "first tap just activates hover" quirk.
  - **Power-outage resilience**: WiFi connection is bounded (8s per
    attempt) and retried every 30s if it fails, rather than blocking
    forever — ESP-NOW/satellite control initializes *before* any WiFi
    attempt, so the hub can still command satellites even if the router
    isn't back yet after a power cut. NTP sync is retried every 30s once
    WiFi is up (covers the router-up-before-internet-up case). Until time
    syncs, the hub runs a free-running fallback clock starting at
    00:00:00 at boot, so the schedule still fires on *some* cadence
    rather than not at all — the page shows "Hora estimada (sin
    sincronizar)" in that state. Caveat: no battery-backed RTC, so this
    fallback resets to 00:00 on every reboot without network access.
- **Pump satellite** (`firmware/pump-satellite/`) — one board built and
  proven, running the real protocol: periodic check-in → hub decides
  run-or-not and sends a command (with a unique id) → satellite runs the
  relay for that duration → sends a report tagged with that id, retried
  every 2s until the hub acks it (satellite gives up after 5 min, harmless
  since watering happens at most 3x/day) → hub acks, dedupes retried
  reports by command id, logs one history entry. No deep sleep
  (intentional, see Power). No calibration *step* in firmware yet — flow
  rate (12.5mL/s, see Reservoir tracking) was measured manually and
  hardcoded on the hub, not self-calibrated by the satellite. No INA219
  dry-run detection yet. Only 1 of the 2 v1 pump satellites is built.
  - **Reliability quirks found and fixed, roughly in the order they bit**:
    (1) the ESP-NOW recv callback must stay fast — calling `delay()` or
    `esp_now_send()` from inside it silently breaks the stack; the actual
    pump run is deferred to `loop()`. (2) Individual sends occasionally
    just don't land — both the report and the command are now sent with
    short-gap redundancy. (3) A run ≥ the check-in interval used to make
    `loop()` fire a check-in immediately after the report send,
    back-to-back in one iteration — the SDK drops one; fixed by resetting
    the check-in timer after a run. (4) The redundant command resends
    weren't deduped on the satellite side, so a second copy arriving
    mid-run queued a second watering right after the first — fixed with
    the same commandId-dedupe pattern already used for reports. (5) **A
    real overflow bug, not just a quirk**: `runMs`/`ranMs` were `uint16_t`
    (max 65,535ms ≈ 65s ≈ 819mL at this flow rate) — raising the UI's mL
    cap to 5000 without widening this backing type meant any dose above
    ~819mL silently computed garbage. Fixed by widening the field to
    `uint32_t` on **both** ends of the wire protocol — a reminder that
    struct-based ESP-NOW messages need type widths checked against actual
    UI-exposed ranges, and that changing them requires reflashing hub and
    satellite together (they must agree on `sizeof()` for the length
    check in `onEspNowRecv` to keep passing).
- **Monitor satellite** — not started (deferred past v1 anyway, see
  Overview).
- **Channel-pinning**: both hub and satellite pin a fixed WiFi channel (1,
  matching the home router) at boot rather than relying on the hub's STA
  connection to set it implicitly — necessary for the outage-resilience
  behavior above (ESP-NOW must work even when the hub isn't associated to
  the router). Still hardcoded, not auto-detected — see the open question
  below, still unsolved if the router ever changes channel.
- **Hardware identification gotcha (real, not hypothetical)**: Linux
  doesn't guarantee stable `/dev/ttyUSB0`/`ttyUSB1` numbering across
  reboots/replugs. After a reboot, firmware got flashed to the wrong
  physical boards purely by port-number assumption (hub firmware landed on
  the relay/pump board and vice versa) — nothing appeared broken in
  software, the pump just silently didn't run. Always verify physically
  (antenna board = hub, relay/pump-wired board = satellite) before
  flashing, e.g. by unplugging one board and checking which `/dev/ttyUSB*`
  disappears, rather than assuming port continuity.
- **Persistence gotcha**: `SatelliteConfig` is saved as one fixed-size
  binary blob under a single NVS key (see firmware.md). Any change to the
  struct's fields (adding `name`, splitting `doseMl` into per-slot
  `slotDoseMl[3]`, etc.) changes its `sizeof()`, which fails the loader's
  size check and silently resets *all* satellite config to defaults on
  next boot — happened more than once today. Not data-corrupting, just
  something to expect (and warn about) whenever a `SatelliteConfig` field
  changes.
- **Real bug found and fixed 2026-08-07**: the schedule appeared to
  "randomly stop working" after some testing, while manual watering kept
  working fine and a hub reboot temporarily fixed it. Root cause: the
  per-slot "already watered today" guard is keyed by slot number, not by
  the slot's configured time — editing a slot's time after it had already
  fired that day (normal while calibrating a schedule) left the guard
  still set, silently suppressing the new time until midnight or reboot.
  Fixed by resetting that guard whenever a schedule is saved. Reproduced
  and confirmed fixed by scheduling two closely-spaced test times on the
  same slot back to back — the second only fired after the fix.
- **`scripts/identify-boards.sh`** now exists to solve the recurring
  `/dev/ttyUSB0`/`ttyUSB1` port-swap problem properly — reads each
  board's real ESP32 MAC via `esptool.py read_mac` (works without the
  firmware even booting) rather than relying on the USB-serial chip's own
  serial number, which turned out to be a non-unique factory default
  shared by every board of this model.
- **Schedule slots: 5 per satellite, not 3** — raised after confirming the
  3-slot version worked correctly (including the guard-reset fix, tested
  with two back-to-back re-schedules on one slot). All 5 slots verified
  firing correctly and in order in one run.
- **Multi-day reliability hardening, 2026-08-07** — added after explicitly
  reasoning through "what happens after this runs unattended for days,"
  not just "does it work right now":
  - **WiFi-drop detection**: previously `wifiConnected` was only ever set
    once, at initial connection — if WiFi dropped *after* that (router
    reboot, ISP hiccup, anything short of the hub itself losing power),
    nothing would notice or retry, ever. Now polls `WiFi.status()` every
    10s and resets the flag if it's no longer connected, letting the
    existing retry-every-30s logic take back over. **Verified**: forced a
    disconnect via a debug endpoint, confirmed the hub went unreachable,
    confirmed it reconnected on its own with no reboot, and — the real
    test — confirmed a *scheduled* watering fired correctly while WiFi
    was still down, proving satellite control genuinely doesn't depend on
    it, not just in theory.
  - **Periodic NTP resync** every 6h even once already synced, as
    insurance against the ESP32's internal clock drifting over a
    multi-day/week trip (only synced once at boot before this).
  - **Preventive daily reboot at 04:00** — clears any `String`-related
    heap fragmentation from hours of page loads/status polls before it
    can turn into an actual crash, rather than waiting to find out.
    Skips itself (retries next cycle) if a satellite is mid-report, so it
    can never cut off an active watering; guarded against re-triggering
    itself immediately after rebooting (the "already rebooted today" flag
    isn't persisted, so a boot landing back in the same minute needed an
    explicit uptime floor).
  - **Free heap exposed on the debug line** — the actual way to know if
    fragmentation is real, by watching the number over days, rather than
    guessing.
  - Router SSID rename wasn't an option (ISP-locked), so added
    `/debug/wifi-disconnect` — an unlinked endpoint that calls
    `WiFi.disconnect()` to simulate the AP vanishing without touching the
    router. Kept as a permanent manual test hook, not removed after use.
  - **Not yet tested, only reasoned about**: actual day-rollover (does a
    slot correctly re-arm after real midnight, not just same-day
    re-scheduling), and heap behavior over many days rather than a few
    test cycles. Both need real elapsed time, not a simulated shortcut —
    the plan is a genuine multi-day soak test with a real schedule
    configured, just letting time pass.

## Overview

**Two layers: the hub, and satellites.** A satellite is either a *pump*
satellite or a *reservoir-monitor* satellite — same node concept, different
role. Both report to the hub; **the hub is the sole decision-maker**. A pump
satellite never decides for itself whether to run — it checks in, the hub
consults everything it knows (schedule, reservoir state), and tells it to
run or not. This keeps all the logic in one place, which is what makes the
hub's local storage (below) the single source of truth for the whole
system.

A reservoir can be shared by several pump satellites; it's watched by
exactly one reservoir-monitor satellite, independent of how many pumps draw
from it.

**v1 scope (must work before early September 2026):** 1 hub + 2 pump
satellites (one balcony, one indoors — the indoor one may feed several pots
from one pump via T-splitters, same dose to all). Monitor satellites,
current sensing (INA219) and the LED panel are designed here but **deferred
past v1** — see each section. The design scales by repetition: adding a
satellite later means flashing identical firmware on another identical
module and registering it on the hub.

```
                    ┌─────────────────────┐
                    │         Hub         │
                    │  owns all state,     │
                    │  makes all decisions │
                    └──────────┬──────────┘
                               │ ESP-NOW — satellites report in,
                               │ hub replies with a command
        ┌──────────────┬───────┴───────┬──────────────┐
        │               │               │              │
  ┌─────▼─────┐   ┌─────▼─────┐   ┌─────▼─────┐  ┌──────▼──────┐
  │  Pump      │   │  Pump      │   │  Pump      │  │  Monitor    │
  │  satellite │   │  satellite │   │  satellite │  │  satellite  │
  │  A         │   │  B         │   │  C  (later)│  │  (deferred) │
  └─────┬──────┘   └─────┬──────┘   └────────────┘  └──────┬──────┘
        │                │                                  │
        └───────draws from tank 1───────┘          watches level of tank 1
```

## Power (decided)

**Everything runs at ~5V from standard USB chargers.** No solar/LiPo
variants in v1 — every node (hub and satellites) sits near a socket with a
5V USB charger. This constraint also drove the pump choice (see Pump
satellite): 5V submersibles keep the whole system on one charger type,
where peristaltics would have forced 12V adapters + buck converters.
Consequences:

- Satellites don't need deep-sleep battery discipline. They can stay awake
  and check in on a simple timer, which removes a whole class of
  wake/sleep/RTC complexity from v1 firmware. Deep sleep remains a future
  optimization if a battery/solar satellite variant ever happens.
- The pump is fed **from the charger's 5V rail through the relay contacts
  (COM/NO), never from the ESP32's 5V pin** (pin limit ~500 mA) — and
  switching the motor on its own rail keeps electrical noise away from the
  board. Wiring through NO (normally open) is fail-safe: relay off or
  unpowered = pump disconnected.
- Common ground between ESP32, relay module and pump supply.

## Hub

- Hardware: **ESP32-WROOM-32U DevKit + external 2.4 GHz U.FL antenna**.
  The hub is the shared node of every wireless link, so it gets the
  antenna upgrade; satellites keep their PCB antennas (see Range).
  ⚠️ Attach the antenna once, before first power-up, then leave it
  connected: an open RF connector reflects transmit power back into the
  amplifier, the U.FL connector tolerates only a few mating cycles, and
  the 32U has no PCB-antenna fallback. Always powered (USB charger).
- **Joins home WiFi as a station**, reachable at a normal LAN address (mDNS
  hostname, e.g. `plant-hub.local`) so it can just be bookmarked in a
  browser. Being WiFi-reliant for the *UI* is fine: the hub's actual
  decision logic (schedule, reservoir state) doesn't depend on that WiFi
  link — the hub↔satellite link is ESP-NOW (see Communication), which
  keeps working independent of the home WiFi/internet being up. If home
  WiFi drops, the bookmark briefly stops loading, but the hub keeps running
  on its last-known settings and keeps making watering decisions exactly
  as before.
- **All state lives on the hub's local flash, full stop** — schedule,
  satellite/reservoir registry (names, which pumps draw from which
  reservoir, capacities, per-pump calibrated flow rate), cumulative-volume
  per reservoir, and the watering log. No cloud dependency. Flash is
  non-volatile, so a power cut doesn't lose it — the hub reloads everything
  on boot and picks up where it left off. (LittleFS for the bulkier
  JSON-ish data, or NVS for small key-value bits; either is fine at this
  data volume.)
- Local web server (ESPAsyncWebServer is the usual choice) serving:
  - Status page: which satellites are online, last-seen time, reservoir
    level estimate / days-until-empty.
  - Schedule page: view/edit per-satellite watering schedule, rename
    satellites and reservoirs, global on/off, "refill" button.
- Enclosure: **plastic, never metal** — metal blocks 2.4 GHz. Antenna end
  clear of metal, batteries and water.

## Satellites

Both roles share the same "brains" board and the same check-in pattern:
on a timer, tell the hub what happened/what they're sensing, receive a
command back, act on it. Neither role stores any decision logic locally
beyond "do what the hub just told me."

Board choice (decided): **ESP32 WROOM-32 DevKit** — same full-size board
family as the hub (PCB-antenna variant; v1 boards are 30-pin ELEGOO
ESP-32S, see [hardware.md](hardware.md)). Not the ESP32-C3 "SuperMini"
boards: their onboard antenna is notoriously weak through walls, and using
one board family everywhere keeps firmware and spares interchangeable.

**Pump satellite**

- Hardware: ESP32 DevKit + **5V relay module with optocoupler**
  (confirmed to trigger from a 3.3V ESP32 GPIO; opto-isolation shields
  the GPIO from coil noise) + **5V mini submersible pump** + USB charger
  power. Relay over MOSFET (decision reversed): proven 3.3V-gate MOSFET
  boards were hard to source, v1 doses by time so PWM isn't needed, and
  contact wear is irrelevant at a few switchings per day. ⚠️ Trigger
  jumper set to HIGH-level and signal on a boot-quiet GPIO (25/26/27,
  never strapping pins 0/2/12/15) so the pump can't blip while the ESP32
  boots.
- Pump type (decided for v1): **5V mini submersible** — cheap (~€4),
  100–200 mA (comfortably USB-powered), high flow (~100 L/h). Chosen over
  peristaltic on cost and 5V availability; trade-offs accepted:
  - **Cannot run dry** — the motor needs water for cooling. This is the
    load-bearing constraint behind Dry-run protection below.
  - **Time-based dosing, not volumetric** — flow varies with water level
    (drops as the reservoir empties), so "run N seconds" delivers a
    shrinking amount over time. Fine for pot plants (±30% tolerance);
    handled with a generous margin in volume tracking.
  - Pump lives inside the reservoir (waterproofed wires), limited head
    height (~0.4–1 m) — reservoir at roughly pot level, and the pump body
    must stay submerged, so the bottom few cm are dead volume.
  - **Upgrade path kept:** peristaltic pumps (precise dosing, dry-safe,
    mounts outside the water) remain the targeted per-module upgrade if
    the project proves itself — pump type is invisible to the
    architecture; upgrading = swap pump + recalibrate that satellite's
    mL/s figure, nothing else changes.
- No float switch on board — reservoir level is the monitor satellite's
  job (deferred), since a reservoir can be shared by several pumps.
- Identified by MAC, assigned to a reservoir and given a friendly name in
  the hub UI (e.g. "Fern — living room"). Purely a hub-side registry edit.
- Behavior: check in with the hub on a timer; hub replies with either
  "don't run" or "run for N seconds"; satellite acts on that instruction,
  reports the outcome, waits for the next cycle.
- Each pump gets a **calibration** at bring-up: run for a fixed time into a
  measuring cup at the reservoir's typical fill level, record mL/s in the
  hub registry; re-check near "low" level to size the dosing margin.

**Monitor satellite (deferred past v1)**

- Design kept for later: ESP32 DevKit + a float switch at the
  "near-empty" level — no pump/relay, it drives nothing. Deliberately
  minimal and DIY/3D-print friendly for the mechanical mounting.
- One monitor per reservoir; a reservoir can feed multiple pump satellites.
- Behavior when built: report `reservoir_id` + empty/not-empty on a timer
  (e.g. every 30–60 min — level changes slowly).
- **Why it can wait for v1:** oversized reservoirs + conservative volume
  tracking cover the vacation scenario; the monitor is the reliability
  upgrade that removes guesswork when it lands.

## Status LEDs (post-v1 nice-to-have)

A physical, at-a-glance status panel on the hub itself — no phone/browser
needed for a quick check. Purely a hub-side feature; doesn't touch
satellite hardware or firmware, since the hub already holds the last-known
status of everything. Not needed for the September deadline; the web status
page covers it. Design kept:

- **Up to 10 LEDs**, one per satellite, plus a **momentary status button**.
  Pressing the button lights the LEDs for a few seconds (e.g. 10s) showing
  a snapshot of current status, then they turn back off.
- **Color = role**: green for pump satellites, blue for monitor satellites.
- **Fixed/solid = working as intended. Blinking = needs attention**:
  offline (no fresh check-in within its expected window), a fault in its
  last report, or a pump that correctly *skipped* watering because its
  reservoir is flagged empty — not a malfunction, but exactly what a
  5-second glance before leaving for a trip should catch.
- LED-to-satellite mapping is a hub-registry setting (an "LED slot" field),
  not automatic — with more than 10 satellites, the important ones get a
  physical LED and the rest stay visible on the web page.
- Hardware recommendation: a single 10-LED **WS2812B addressable strip** on
  one GPIO data line rather than 10 discrete LEDs on 10 GPIOs — color and
  blink become software-defined per LED.

## Dry-run protection

Cheap submersible pumps aren't rated to run dry — the motor relies on the
water for cooling, so an empty reservoir can burn one out. The hub is the
sole decision-maker (see Satellites), so this logic lives on the hub. Two
things soften the problem in v1 compared to the original worst case:
pumps cost ~€4 (sacrificial, spare on hand), and the primary mitigation is
physical, not clever — **oversize the reservoirs** so they mathematically
can't empty during a trip.

The design goal, kept from the original: a failing information source
should degrade gracefully, not take plants down with it. v1 layers:

1. **Oversized reservoirs** — sized so a full vacation's schedule uses well
   under capacity. First and best line of defense; costs nothing.
2. **Volume tracking with a conservative margin.** The hub tracks
   cumulative dispensed volume per reservoir (below). When the
   margin-adjusted remaining volume hits zero, the hub marks the reservoir
   empty, stops scheduling every pump attached to it, and flags it in the
   UI. The margin is generous because time-based dosing with submersibles
   drifts as the level drops.
3. **"Refill" in the UI** resets the counter and clears the empty flag
   after a physical top-up.

Deferred layers (kept for later, slot into the same hub logic unchanged):

- **Monitor satellite** (float switch): a fresh reading takes priority
  over the estimate; a fresh "not-empty" clears the empty flag; a stale
  monitor falls back to the margin-adjusted estimate.
- **INA219 current sensing** per pump satellite: real-time dry detection
  (an unprimed/dry pump has a different current signature) plus diagnostic
  "pump detected" — catches faults nothing else can see, like clogged
  intake or a dead motor ("commanded on, drew ~0 mA"). With submersibles
  this guards actual hardware, so it's the first deferred item worth
  revisiting if a pump ever burns out in practice.

## Reservoir tracking & logging

- Each reservoir (not each satellite — reservoirs can be shared) has a
  configured **capacity** (e.g. 8000 mL) and name, editable in the hub UI.
  Configure *usable* capacity: submersible pumps must stay covered, so the
  bottom few cm don't count.
- The hub keeps a running **cumulative volume dispensed since last refill**
  per reservoir: every watering event from *any* satellite drawing on that
  reservoir adds `duration_run × that pump's calibrated flow rate (mL/s)`
  to the reservoir's total. An estimate (no flow sensor), and a drifting
  one with submersibles — hence the generous margin in Dry-run protection.
- A **"refill" action** in the hub UI resets a reservoir's cumulative
  dispensed total to zero.
- **Days-until-empty** per reservoir is computed by projecting the
  *upcoming* schedule of every satellite attached to it forward against
  remaining volume, rather than only averaging past usage — gives a
  sensible number immediately after a refill, before any history exists.
  This same figure, with the safety margin applied, is what drives the
  empty flag in Dry-run protection — so it's load-bearing, not just a UI
  convenience.
- **Log retention: 45 days, then deleted.** The hub logs every watering
  event (satellite, reservoir, timestamp, duration, estimated volume,
  outcome) to flash; entries older than 45 days are purged outright — no
  cloud offload, since remote access (below) is deferred. Revisit sizing
  once we know actual event volume and flash headroom on the real board.

## Remote access (deferred — design kept for later, not being built now)

Not a v1 requirement — the near-term goal is getting the local system
working first. Keeping the design here so the *local* system is built in a
way that doesn't have to be reworked when this gets added:

- Decision when we do build it: **outbound relay**, not direct
  port-forwarding. The hub makes an outbound connection to a small
  always-on relay rather than accepting inbound internet connections — no
  port opened on the home router, works regardless of ISP CGNAT.
- Hub already being WiFi-station-connected in v1 (see Hub section) means
  this can be added later as a new outbound client (e.g. MQTT over TLS to a
  self-hosted broker on a cheap VPS, or a managed free-tier broker) without
  touching the hub's network mode.
- Hub would publish satellite/reservoir statuses and log events, and
  subscribe to remotely-issued schedule/command changes. A relay-side
  companion service would persist history to its own database and serve a
  remote dashboard — none of which needs to exist until this phase starts.
- Security when built: unique credentials/cert per hub, TLS throughout,
  authenticated command topic.

## Communication: hub ↔ satellites (decided: ESP-NOW)

Applies to both pump and (future) monitor satellites — same link, same
trade-offs. All satellite check-ins are **hub-mediated only** (a satellite
always talks to the hub, never to another satellite directly) — that's what
makes the hub the single decision-maker in practice, not just on paper.

**Decision: ESP-NOW.** With everything USB-powered (see Power), the
original battery-life argument matters less — the deciding reasons now are:

- The hub↔satellite link stays independent of the home router: tiny
  connectionless packets, no association/DHCP dance, no router in the loop
  for the control path.
- Fast, simple check-in cycle; robust for small command packets even over
  links too weak for streaming.
- Scales by repetition: new satellite = same firmware, new MAC registered
  on the hub.

The hub bridges ESP-NOW messages into its own web UI/state. The rejected
alternatives (WiFi station + HTTP per satellite, MQTT on the hub) are noted
in git history; both put the home router inside the watering control loop.

Real gotcha to plan for: when the hub's WiFi radio is also associated to
home WiFi as a station (see Hub section), ESP-NOW traffic shares that same
2.4 GHz channel — the hub can't be on a different channel for ESP-NOW than
its WiFi STA connection. In practice this means the satellites must follow
the home router's channel, and handling the case where the router changes
channel (rare, but routers do this on their own sometimes). Doesn't affect
the "keeps working without internet" property — that's about the WAN link,
not the channel — but it's a real implementation detail to not gloss over.

## Range (decided strategy)

- **Hub:** 32U + external antenna — the central node, so boosting it helps
  every link at once.
- **Satellites:** standard PCB antennas. ESP-NOW's tiny packets survive
  links that would choke a webpage; through 2–3 interior walls the PCB
  antenna is normally enough.
- **Contingency:** one spare 32U + antenna as an optional add-on to the
  parts order. First thing after bring-up (before any enclosures): flash an
  ESP-NOW ping sketch and walk each satellite to its real spot (balcony,
  indoor location). If a location is flaky, swap that one satellite to the
  spare 32U — no over-buying up front.

## Open questions

- [x] Hub network mode: **joins home WiFi (station only)** for v1, reachable
      via mDNS. No AP for now — see Hub section for why, and how this sets
      up remote access later without a rework.
- [x] Module power source: **resolved as USB chargers everywhere** for v1 —
      see Power. Solar+LiPo variant shelved with deep-sleep firmware as a
      possible future satellite type.
- [x] Pump type: **resolved as 5V mini submersible for v1** (cost +
      5V-availability; peristaltic kept as the per-module upgrade path) —
      see Pump satellite. Dry-run protection is therefore
      hardware-survival: oversized reservoirs + conservative volume margin.
- [x] What does "pump detected" mean exactly — INA219 current sensing,
      **deferred past v1**; first deferred item to revisit if a pump ever
      burns out. See Dry-run protection.
- [x] Water source per module: **resolved as shared reservoirs**; monitor
      satellites deferred, v1 covers it with oversized reservoirs + volume
      tracking + margin.
- [x] Communication: **resolved as ESP-NOW**, hub-mediated only — see
      Communication section.
- [x] Range strategy: **resolved** — 32U hub + PCB-antenna satellites +
      optional contingency 32U + range test before enclosures. See Range.
- [ ] ESP-NOW + WiFi-STA channel coupling on the hub (see Communication
      section) — decide how satellites detect/follow a router channel
      change. Candidate: satellites scan for the hub if N check-ins fail.
- [ ] Range test results pending hardware arrival — confirms whether the
      contingency 32U gets used.
- [ ] Log sizing on real flash once event volume is known (45-day retention
      assumption).
- [x] Pump driver: **resolved as 5V opto-isolated relay module** (MOSFET
      plan dropped — sourcing a proven 3.3V-gate one was the last blocker
      and v1 needs no PWM). See Pump satellite and
      [hardware.md](hardware.md).

## Phase 2 (winter): sensing

- Add humidity + temperature sensors per pump satellite (e.g. capacitive
  soil moisture sensor + a cheap temp/humidity combo like DHT22 or SHT31).
- Hub schedule logic becomes conditional: skip/shorten watering if soil
  moisture is already high, extend on hot/dry days.
- No architecture changes needed beyond adding sensor reads to the
  satellite's check-in cycle and reporting the values — the wireless link
  and hub schedule model already support it, since the hub already makes
  every watering decision.
- Same window: optional peristaltic pump upgrades and INA219 current
  sensing (see Pump satellite / Dry-run protection).
