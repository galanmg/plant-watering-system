# Architecture

Status: draft, nothing built yet. This doc is meant to be updated as decisions
get made — treat "Open questions" as the running to-do list.

## Overview

Three node types: the hub, pump satellites, and reservoir monitors. A
reservoir can be shared by several pump satellites; it's watched by exactly
one reservoir monitor, independent of how many pumps draw from it.

```
                    ┌─────────────────────┐
                    │         Hub         │
                    │  ESP32 + web server │
                    └──────────┬──────────┘
                               │ wireless
        ┌──────────────┬───────┴───────┬──────────────┐
        │               │               │              │
  ┌─────▼─────┐   ┌─────▼─────┐   ┌─────▼─────┐  ┌──────▼──────┐
  │  Pump      │   │  Pump      │   │  Pump      │  │  Reservoir  │
  │  satellite │   │  satellite │   │  satellite │  │  monitor    │
  │  A         │   │  B         │   │  C         │  │  (tank 1)   │
  └─────┬──────┘   └─────┬──────┘   └────────────┘  └──────┬──────┘
        │                │                                  │
        └───────draws from tank 1───────┘          watches level of tank 1
```

## Hub

- Hardware: ESP32 (WROOM or similar dev board), always powered (plugged in).
- **v1 network mode: joins home WiFi as a station**, reachable at a normal
  LAN address (mDNS hostname, e.g. `plant-hub.local`, so no need to hunt for
  an IP). No separate AP for now — simpler to build, and since remote access
  (below) is coming later and needs an internet path anyway, starting on
  home WiFi means adding that later doesn't require a network-mode change.
  Trade-off accepted: if home WiFi is down, the local status page is briefly
  unreachable too — acceptable since dry-run safety doesn't depend on the
  hub anyway (see Dry-run protection).
- Local web server (ESPAsyncWebServer is the usual choice) serving:
  - Status page: which pump satellites and reservoir monitors are online,
    last-seen time, battery level, reservoir level / days-until-empty.
  - Schedule page: view/edit per-satellite watering schedule, rename
    satellites and reservoirs, global on/off.
  - Simple REST-ish endpoints so satellites/monitors can report in and pull
    their schedule/config.
- Persists schedule, satellite/reservoir registry (names, which satellites
  draw from which reservoir, reservoir capacities), and the watering log to
  flash (e.g. LittleFS/SPIFFS + JSON, or NVS) so a power cycle doesn't lose
  state.

## Pump satellites

- Hardware per satellite: ESP32-C3 + MOSFET or relay driving a small
  submersible/peristaltic pump + power stage (outlet or solar/battery — see
  [hardware.md](hardware.md)). No float switch on the pump satellite itself —
  reservoir level is the monitor's job (below), since a reservoir can be
  shared by several pump satellites.
- Each satellite is identified by a stable ID (its MAC address), assigned to
  a reservoir (which one it draws from), and given a human-friendly name in
  the hub UI once it's fixed to a specific plant (e.g. "Fern — living
  room"). Naming/assignment is purely a hub-side registry edit; the
  satellite itself doesn't need to know either.
- Behavior (Phase 1): wake up on a timer, ask the hub (or, see open question
  below, listen directly) whether its assigned reservoir is currently known
  non-empty and that status is fresh; if so and watering is due, run the
  pump for the scheduled duration; report status (ran / skipped-empty /
  skipped-stale) back to the hub; go back to deep sleep.
- Behavior (Phase 2 addition): also read humidity/temperature sensor(s)
  before deciding how much to water, and report readings to the hub.

## Reservoir monitors

Auxiliary, pump-less nodes: one per physical container, regardless of how
many pump satellites draw from it. Deliberately minimal and DIY-friendly —
just enough electronics to sense a level and report it, with the mechanical
mounting (float switch bracket, enclosure) left as a build/3D-print detail
per container shape, not a fixed part spec.

- Hardware: ESP32-C3 + a float switch at the "near-empty" level + a power
  stage (outlet or battery — no pump/MOSFET/current-sensing needed here,
  since it drives nothing). See [hardware.md](hardware.md).
- Behavior: wake on a timer *more often than pump satellites typically
  water* (e.g. every 30–60 minutes — level changes slowly, and checking a
  switch + reporting is cheap), read the float switch, report
  `reservoir_id`, empty/not-empty, timestamp, back to the hub. Sleep.
- Identified by MAC, assigned a human-friendly reservoir name and capacity
  in the hub UI (e.g. "Kitchen jerrycan — 8 L").
- A reservoir can have multiple pump satellites pointed at it; it has
  exactly one monitor.

## Dry-run protection

Cheap submersible pumps aren't rated to run dry — the motor relies on the
water for cooling/lubrication, so an empty reservoir can burn one out.
Splitting monitoring from pumping (previous section) means the pump
satellite can no longer read a float switch on its own board — the "empty"
signal now necessarily crosses a wireless hop. Layers, in order of how much
they can be trusted:

1. **Float switch on the reservoir monitor (hardware)** — the actual sensing
   is still a dumb physical switch, as reliable as before. What's changed is
   *who* has it: the monitor, not the pump satellite.
2. **Freshness/fail-safe default (critical, software)** — because the empty
   signal now has to travel over the network to reach the pump satellite, a
   pump satellite must treat a *stale* reservoir status (monitor hasn't
   reported in longer than expected, hub unreachable, etc.) the same as
   "empty": **default to skipping the watering, not running it**, whenever
   it can't confirm a recent non-empty reading. This preserves a fail-safe
   posture despite no longer being a same-board GPIO read.
3. **Current sensing (diagnostic, on the pump satellite)** — the INA219
   already planned for "pump detected" (see [hardware.md](hardware.md))
   catches faults the float switch can't, like clogged tubing that starves
   flow even though the reservoir itself isn't empty.
4. **Volume tracking (convenience, software)** — see below. A "fuel gauge"
   for planning ahead, not a safety mechanism.

Open implementation question: should a pump satellite get the reservoir's
status only via the hub (simpler — one request per wake, pulls schedule +
reservoir status together), or listen directly for the monitor's periodic
broadcast when in range (more resilient — still works during a brief hub
outage, at the cost of satellites needing to know which monitor's ID to
listen for)? Leaning towards starting with the hub-mediated version for
simplicity, given the freshness fail-safe already covers the "hub is down"
case safely (it just waters less eagerly, never unsafely).

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

## Communication: hub ↔ satellites/monitors

This is the biggest open decision. Applies to both pump satellites and
reservoir monitors — same link, same trade-offs. Options, given the hub
needs to host its own WiFi + web UI and battery-powered nodes are the norm:

| Option | Pros | Cons |
|---|---|---|
| **WiFi station + deep sleep** (module joins hub's AP like a normal WiFi client, does an HTTP request, sleeps) | Simple, reuses the hub's existing web server/HTTP stack, easy to debug (it's just HTTP) | WiFi join + handshake costs real power/time even with deep sleep; range inside a house with walls may be marginal for far rooms |
| **ESP-NOW** (connectionless, low-power peer-to-peer WiFi protocol) | Very low power, fast wake-send-sleep cycle (~ms), no association overhead, good range for line-of-sight | No native multi-hop/relay (would need to hand-roll if range is an issue); hub needs a small bridge layer between ESP-NOW and the web UI |
| **MQTT over WiFi** (modules publish to a broker, e.g. running on the hub) | Clean pub/sub model, easy to extend, good tooling | Same WiFi power cost as option 1, plus a broker to run on the hub |

Leaning towards **ESP-NOW** given battery life matters more than protocol
elegance, with the hub bridging ESP-NOW messages into its own web UI/state.
Worth prototyping both ESP-NOW and plain WiFi+deep-sleep power draw before
committing, though.

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
      from pump satellites via dedicated reservoir monitors — see Reservoir
      monitors section. A reservoir can feed multiple pump satellites.
- [ ] Range: how far are the farthest plants from where the hub would live?
      Determines whether ESP-NOW range is sufficient or a relay module is
      needed.
- [ ] Reservoir status delivery to pump satellites: via the hub only, or
      also direct monitor→satellite broadcast for resilience? See open
      question at the end of Dry-run protection.
- [x] Remote access while on vacation: **deferred** — design kept in the
      Remote access section for later, not being built in v1. Local system
      built so adding it later doesn't require rework (hub already on WiFi
      station mode).

## Phase 2 (later): sensing

- Add humidity + temperature sensors per module (e.g. capacitive soil
  moisture sensor + a cheap temp/humidity combo like DHT22 or SHT31).
- Hub schedule logic becomes conditional: skip/shorten watering if soil
  moisture is already high, extend on hot/dry days.
- No architecture changes needed for this beyond adding sensor reads to the
  module's wake cycle and reporting the values — the wireless link and hub
  schedule model already support it.
