# Architecture

Status: draft, nothing built yet. This doc is meant to be updated as decisions
get made — treat "Open questions" as the running to-do list.

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

```
                    ┌─────────────────────┐
                    │         Hub         │
                    │  owns all state,     │
                    │  makes all decisions │
                    └──────────┬──────────┘
                               │ wireless — satellites report in,
                               │ hub replies with a command
        ┌──────────────┬───────┴───────┬──────────────┐
        │               │               │              │
  ┌─────▼─────┐   ┌─────▼─────┐   ┌─────▼─────┐  ┌──────▼──────┐
  │  Pump      │   │  Pump      │   │  Pump      │  │  Monitor    │
  │  satellite │   │  satellite │   │  satellite │  │  satellite  │
  │  A         │   │  B         │   │  C         │  │  (tank 1)   │
  └─────┬──────┘   └─────┬──────┘   └────────────┘  └──────┬──────┘
        │                │                                  │
        └───────draws from tank 1───────┘          watches level of tank 1
```

## Hub

- Hardware: ESP32 (WROOM or similar dev board), always powered (plugged in).
- **Joins home WiFi as a station**, reachable at a normal LAN address (mDNS
  hostname, e.g. `plant-hub.local`) so it can just be bookmarked in a
  browser. Being WiFi-reliant for the *UI* is fine: the hub's actual
  decision logic (schedule, reservoir state, dry-run handling) doesn't
  depend on that WiFi link — the hub↔satellite link is its own wireless
  channel (see Communication section) that keeps working independent of
  the home WiFi/internet being up. If home WiFi drops, the bookmark
  briefly stops loading, but the hub keeps running on its last-known
  settings and keeps making watering decisions exactly as before.
- **All state lives on the hub's local flash, full stop** — schedule,
  satellite/reservoir registry (names, which pumps draw from which
  reservoir, capacities), cumulative-volume-per-reservoir, and the watering
  log. No cloud dependency for any of this. Flash is non-volatile, so a
  power cut doesn't lose it — the hub reloads everything on boot and picks
  up exactly where it left off. (LittleFS/SPIFFS for the bulkier
  JSON-ish data, or NVS for small key-value bits; either is fine at this
  data volume — this was never a reason to reach for cloud storage, that
  was only ever discussed for keeping history *beyond* the local retention
  window, see Remote access.)
- Local web server (ESPAsyncWebServer is the usual choice) serving:
  - Status page: which pump satellites and reservoir monitors are online,
    last-seen time, battery level, reservoir level / days-until-empty.
  - Schedule page: view/edit per-satellite watering schedule, rename
    satellites and reservoirs, global on/off.
  - Simple REST-ish endpoints satellites hit when they check in.

## Satellites

Both roles share a "brains" board (ESP32-C3) and the same check-in pattern:
wake on a timer, tell the hub what happened/what they're sensing, receive a
command back, act on it, sleep. Neither role stores any decision logic
locally beyond "do what the hub just told me."

**Pump satellite**

- Hardware: ESP32-C3 + MOSFET or relay driving a small submersible pump +
  power stage (outlet or solar/battery — see [hardware.md](hardware.md)).
  No float switch on board — reservoir level is the monitor satellite's
  job, since a reservoir can be shared by several pumps.
- Identified by MAC, assigned to a reservoir and given a friendly name in
  the hub UI (e.g. "Fern — living room"). Purely a hub-side registry edit.
- Behavior: wake on a timer, check in with the hub; hub replies with either
  "don't run" or "run for N seconds" (optionally flagged with "dry-check
  enabled" — see Dry-run protection); satellite acts on that instruction,
  reports the outcome, sleeps.

**Monitor satellite**

- Hardware: ESP32-C3 + a float switch at the "near-empty" level + a power
  stage — no pump/MOSFET/current-sensing, it drives nothing. Deliberately
  minimal and DIY/3D-print friendly for the mechanical mounting, which
  varies per container shape. See [hardware.md](hardware.md).
- Identified by MAC, assigned a friendly reservoir name and capacity in the
  hub UI (e.g. "Kitchen jerrycan — 8 L"). One monitor per reservoir, but a
  reservoir can feed multiple pump satellites.
- Behavior: wake more often than pumps typically water (e.g. every 30–60
  minutes — level changes slowly, and a switch read + report is cheap),
  report `reservoir_id` + empty/not-empty + timestamp to the hub, sleep.

## Status LEDs

A physical, at-a-glance status panel on the hub itself — no phone/browser
needed for a quick check. Purely a hub-side feature; doesn't touch
satellite hardware or firmware, since the hub already holds the
last-known status of everything (see Hub section).

- **Up to 10 LEDs**, one per satellite, plus a **momentary status button**.
  Pressing the button lights the LEDs for a few seconds (e.g. 10s) showing
  a snapshot of current status, then they turn back off — not an
  always-on display.
- **Color = role**: green for pump satellites, blue for monitor satellites.
- **Fixed/solid = working as intended.** **Blinking = needs attention.**
  Concretely, "needs attention" means any of:
  - the hub hasn't had a fresh check-in from that satellite within its
    expected window (offline);
  - its last report included a fault (a pump's `aborted-dry` outcome, an
    INA219 reading of "commanded on, drew ~0mA", a monitor stuck reporting
    the same value past its own staleness window);
  - a pump satellite correctly *skipped* watering because its reservoir is
    flagged empty. Not a malfunction, but it's exactly the kind of thing a
    5-second glance before leaving for a trip should catch — so it blinks
    too, same as an actual fault.
- **Which LED maps to which satellite is a hub-registry setting** (an "LED
  slot" field alongside a satellite's name), not automatic/first-come — so
  if there are ever more than 10 satellites, whichever ones matter most get
  a physical LED and the rest are still visible on the web status page.
- Hardware recommendation (see [hardware.md](hardware.md)): a single
  10-LED **WS2812B addressable strip** on one GPIO data line, rather than
  10 discrete LEDs on 10 GPIOs — color and blink state become entirely
  software-defined per LED, so the green/blue split isn't wired in at
  build time and can be rebalanced in software as the pump/monitor mix
  changes.

## Dry-run protection

Cheap submersible pumps aren't rated to run dry — the motor relies on the
water for cooling/lubrication, so an empty reservoir can burn one out. The
hub is the sole decision-maker here (see Satellites), so all of this logic
lives on the hub, not spread across satellites.

The design goal isn't just "never run a pump dry" — it's that a **failing
monitor should degrade gracefully, not take plants down with it**. Treating
every stale monitor reading as "assume empty, stop watering" would be safe
for the pump but not for the plant: a dead monitor battery or a wireless
glitch would quietly starve every plant on that reservoir for the whole
trip. So instead:

1. **Normal case — fresh float switch reading.** Monitor reports
   empty/not-empty on schedule; the hub trusts a fresh reading directly.
   Not-empty → scheduled pumps on that reservoir run normally.
2. **Monitor goes stale (hasn't reported within its expected window).**
   Rather than assuming empty, the hub falls back to its own volume-tracking
   estimate for that reservoir (below), applied with a **margin of error**
   — i.e. it treats the reservoir as empty somewhat earlier than the raw
   estimate would suggest, to absorb drift in the estimate. As long as the
   margin-adjusted estimate still shows water left, the hub keeps letting
   pumps run on schedule.
3. **Whenever a pump runs while its reservoir's monitor is stale, the hub
   enables a dry-check on that run.** The pump satellite starts the pump
   and watches its INA219 current draw for the first ~1–2 seconds; if the
   signature doesn't look primed, it **self-aborts immediately** rather
   than completing the scheduled duration, and reports `aborted-dry`
   instead of `ran`. This has to be a local, real-time decision on the
   satellite itself — a mid-run round trip to the hub over a wireless link
   isn't fast or reliable enough to gate a decision that needs to happen
   within ~1–2 seconds.
4. **The instant any pump reports `aborted-dry`, the hub marks that whole
   reservoir empty**, overriding both the stale monitor and the volume
   estimate. Because the empty flag lives on the reservoir (not the
   individual pump), this immediately protects every other pump satellite
   sharing that container too — next check-in, the hub tells them not to
   run. Net effect: worst case is one pump, one brief (1–2s) dry attempt,
   and every pump on that reservoir is protected from then on.
5. **Recovery**: once the monitor resumes reporting, a fresh reading takes
   priority again over the estimate/aborted-dry state — a fresh
   "not-empty" clears the empty flag (the user can also always clear it
   manually via "refill" in the UI after physically topping up).
6. **Current sensing (diagnostic, independent of the above)** — the INA219
   also just generally answers "pump detected" (see
   [hardware.md](hardware.md)): a way to catch faults the float switch
   never could, like clogged tubing starving flow even though the
   reservoir itself isn't empty.
7. **Volume tracking (below)** — normally just a "days left" convenience
   figure; becomes load-bearing (with the margin from step 2) the moment a
   monitor degrades.

## Reservoir tracking & logging

- Each reservoir (not each satellite — reservoirs can be shared) has a
  configured **capacity** (e.g. 8000 mL) and name, editable in the hub UI.
- The hub keeps a running **cumulative volume dispensed since last refill**
  per reservoir: every watering event from *any* satellite drawing on that
  reservoir adds `duration_run × that pump's nominal flow rate (mL/s at
  5V)` to the reservoir's total. Estimate, not a precise measurement (no
  flow sensor in the current plan) — the "days left" number is an
  approximation.
- A **"refill" action** in the hub UI resets a reservoir's cumulative
  dispensed total to zero.
- **Days-until-empty** per reservoir is computed by projecting the
  *upcoming* schedule of every satellite attached to it forward against
  remaining volume, rather than only averaging past usage — gives a
  sensible number immediately after a refill, before any history exists.
  This same remaining-volume figure, with a safety margin applied, is what
  the hub falls back on if the reservoir's monitor goes stale (see
  Dry-run protection step 2) — so it's not purely a UI convenience number,
  it's also the load-bearing estimate during a monitor outage.
- **Log retention: 45 days, then deleted.** The hub logs every watering
  event (satellite, reservoir, timestamp, duration, estimated volume,
  outcome) to flash; entries older than 45 days are purged outright — no
  cloud offload, no rolling summarization, since remote access (below) is
  deferred and there's no second place to keep them anyway. Revisit sizing
  once we know actual event volume and flash headroom on the real board.

## Remote access (deferred — design kept for later, not being built now)

Not a v1 requirement — the near-term goal is getting the local system
(hub + satellites + monitors, on home WiFi) working first. Keeping the
design here so the *local* system is built in a way that doesn't have to
be reworked when this gets added:

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

## Communication: hub ↔ satellites

This is the biggest open decision. Applies to both pump and monitor
satellites — same link, same trade-offs. All satellite check-ins are
**hub-mediated only** (a satellite always talks to the hub, never to
another satellite directly) — that's what makes the hub the single
decision-maker in practice, not just on paper.

| Option | Pros | Cons |
|---|---|---|
| **WiFi station + deep sleep** (satellite joins hub's AP like a normal WiFi client, does an HTTP request, sleeps) | Simple, reuses the hub's existing web server/HTTP stack, easy to debug (it's just HTTP) | WiFi join + handshake costs real power/time even with deep sleep; range inside a house with walls may be marginal for far rooms |
| **ESP-NOW** (connectionless, low-power peer-to-peer WiFi protocol) | Very low power, fast wake-send-sleep cycle (~ms), no association overhead, good range for line-of-sight | No native multi-hop/relay (would need to hand-roll if range is an issue); hub needs a small bridge layer between ESP-NOW and the web UI |
| **MQTT over WiFi** (satellites publish to a broker, e.g. running on the hub) | Clean pub/sub model, easy to extend, good tooling | Same WiFi power cost as option 1, plus a broker to run on the hub |

Leaning towards **ESP-NOW** given battery life matters more than protocol
elegance, with the hub bridging ESP-NOW messages into its own web UI/state.
Worth prototyping both ESP-NOW and plain WiFi+deep-sleep power draw before
committing, though.

Real gotcha to plan for: when the hub's WiFi radio is also associated to
home WiFi as a station (see Hub section), ESP-NOW traffic shares that same
2.4GHz channel — the hub can't be on a different channel for ESP-NOW than
its WiFi STA connection. In practice this means pinning/tracking the home
router's channel for the ESP-NOW link, and handling the case where the
router changes channel (rare, but routers do this on their own sometimes).
Doesn't affect the "keeps working without internet" property — that's
about the WAN link, not the channel — but it's a real implementation detail
to not gloss over.

## Open questions

- [x] Hub network mode: **joins home WiFi (station only)** for v1, reachable
      via mDNS. No AP for now — see Hub section for why, and how this sets
      up remote access later without a rework.
- [x] Module power source: outlet-powered where possible, solar+LiPo where
      not — see [hardware.md](hardware.md), both variants regulated to a
      common 5V rail so the rest of the circuit is power-source-agnostic.
- [x] What does "pump detected" mean exactly — draft answer in
      [hardware.md](hardware.md): an inline INA219 current sensor, so the
      satellite can tell the hub "commanded on but drew ~0mA" vs. "drew
      expected current," not just a heartbeat. Still open whether this is
      in scope for v1 or deferred (see hardware.md's open items).
- [x] Water source per module: **resolved as shared reservoirs**, decoupled
      from pump satellites via dedicated monitor satellites — see
      Satellites section. A reservoir can feed multiple pump satellites.
- [ ] Range: how far are the farthest plants from where the hub would live?
      Determines whether ESP-NOW range is sufficient or a relay module is
      needed.
- [x] Reservoir status delivery to pump satellites: **resolved as
      hub-mediated only** — a satellite never talks to another satellite
      directly, only to the hub, which is what makes the hub the actual
      sole decision-maker. See Communication section.
- [x] Remote access while on vacation: **deferred** — design kept in the
      Remote access section for later, not being built in v1. Local system
      built so adding it later doesn't require rework (hub already on WiFi
      station mode).
- [ ] ESP-NOW + WiFi-STA channel coupling on the hub (see Communication
      section) — need to decide how to keep the ESP-NOW link stable if the
      home router changes its WiFi channel.

## Phase 2 (later): sensing

- Add humidity + temperature sensors per pump satellite (e.g. capacitive
  soil moisture sensor + a cheap temp/humidity combo like DHT22 or SHT31).
- Hub schedule logic becomes conditional: skip/shorten watering if soil
  moisture is already high, extend on hot/dry days.
- No architecture changes needed for this beyond adding sensor reads to the
  satellite's wake cycle and reporting the values — the wireless link and
  hub schedule model already support it, since the hub already makes every
  watering decision.
