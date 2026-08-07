# Firmware

Status (2026-08-07): hub and one pump satellite built and flashed, running
the real check-in/command/ack/report protocol, a persisted multi-satellite
registry with per-slot (up to 5x/day) independent dosing, a live-updating
web UI, and hardening aimed specifically at multi-day unattended
reliability (WiFi-drop detection, periodic resync, preventive daily
reboot). See architecture.md's "Current implementation status" for what
each one actually does today.

## Toolchain

PlatformIO (CLI), installed via `pipx install platformio`. Each firmware
target is its own PlatformIO project under `firmware/<target>/` — a
separate `platformio.ini`/`src/`/`include/` per board, not one shared
project, since hub and satellites are different roles (and originally
different board types, before the C3→WROOM-32 substitution — see
hardware.md).

Build + flash a target:
```bash
cd firmware/<target>
pio run -t upload                       # auto-detects the port
pio run -t upload --upload-port /dev/ttyUSB1   # or specify, if >1 board is plugged in
```

Serial monitor: `pio device monitor -p /dev/ttyUSBx` — must be run from a
real interactive terminal (fails with a termios error under a
non-interactive/sandboxed shell).

## Known quirks

- **Serial permissions (Arch/CachyOS)**: this distro uses the `uucp` group
  for `/dev/ttyUSB*`, not `dialout` like Debian-based systems.
  `sudo usermod -aG uucp $USER`, then log out/in.
- **Upload speed**: the ELEGOO/CP2102 boards used here fail upload at
  esptool's default 460800 baud (`Unable to verify flash chip connection`
  / `No serial data received`) after the stub handoff. Fixed by pinning
  `upload_speed = 115200` in `platformio.ini` — slower but reliable. Set in
  both `firmware/hub/platformio.ini` and `firmware/pump-satellite/platformio.ini`.
- **Board target**: both the hub (WROOM-32U) and satellites (WROOM-32) use
  PlatformIO's `esp32dev` board id — the "U" antenna variant and the
  ELEGOO satellite boards don't need a different target, same chip.
- **ESP-NOW callback signature**: the installed arduino-esp32 core
  (3.20017.x) uses the *old* callback signature —
  `void onRecv(const uint8_t *mac, const uint8_t *data, int len)` — not the
  newer `esp_now_recv_info_t*`-based one some docs/examples show. Mismatch
  fails to compile with a clear pointer-type error, easy to fix once you
  know which signature the installed core version expects.
- **`/dev/ttyUSB0`/`ttyUSB1` numbering is not stable across reboots or
  replugs — confirmed again on 2026-08-07, the mapping was the exact
  opposite of the day before.** After a reboot, assuming "port 0 is still
  the hub" led to flashing firmware onto the wrong physical boards —
  everything compiled and ran, the hub even booted and served its page
  fine, it just silently talked to itself instead of the satellite.
  Run **`scripts/identify-boards.sh`** before flashing after any
  reboot/replug — it reads each board's real hardware MAC via
  `esptool.py read_mac` (works against the ROM bootloader, no firmware
  needs to be running) and matches it against the known hub/satellite
  MACs, so no unplug-and-check-what-disappears dance is needed. Note the
  CP2102 USB-serial chips on these boards report the *same* generic
  factory serial (`0001`) on every board — that field can't be used to
  tell them apart, which is why the script reads the ESP32's own MAC
  instead, a genuine per-chip identifier. (Hub = the board with the
  antenna; satellite = the board wired to the relay/pump — for when a
  truly new/unknown board shows up and needs adding to the script.)
- **Debugging without reliable serial access**: reading `/dev/ttyUSB*`
  programmatically (not via an interactive `pio device monitor` in a real
  terminal) was unreliable in this setup — reads intermittently returned
  garbled or duplicated data, sometimes at a byte rate physically
  impossible for the actual baud rate. When debugging remotely, prefer
  exposing state via the hub's web page (it already has a "Debug ESP-NOW"
  line with packet counts/types/senders) over trusting a scripted serial
  capture; ask the user to check `pio device monitor` themselves in their
  own terminal when satellite-side serial output is actually needed.
- **ESP-NOW reliability, found while wiring up the real protocol** (see
  architecture.md's "Current implementation status" for the fuller
  writeup): (1) never call `delay()` or `esp_now_send()` from inside the
  recv callback — defer to `loop()` via a pending-flag instead, or sends
  silently misbehave. (2) Individual `esp_now_send()` calls occasionally
  just don't arrive, no error reported — send anything that matters
  (like a run report or command) 2-3x with a short gap. (3) Two
  `esp_now_send()` calls back-to-back in the same `loop()` iteration can
  result in one being dropped — if a blocking operation (like running a
  pump) can push a periodic send (like a check-in) to fire immediately
  afterward, reset that periodic timer explicitly rather than letting the
  two collide. (4) Redundant resends of the *same* logical message need
  dedup on the receiving end (by a commandId/counter) — the hub already
  deduped retried reports, but the satellite didn't dedupe retried
  commands at first, so a resend arriving mid-run queued a second run
  right after the first (a real double-watering bug, not hypothetical).
- **Struct field widths need checking against actual UI-exposed
  ranges, not just "seems big enough."** `runMs`/`ranMs` were `uint16_t`
  (max ~65s ≈ 819mL at this flow rate) — fine when doses were capped at
  1000mL in the UI (already silently wrong above 819!), and only surfaced
  as a visible bug once the cap was raised to 5000mL and someone actually
  requested 2000mL. Widened to `uint32_t` on both `CommandMsg` and
  `ReportMsg`. Since these are raw structs sent byte-for-byte over
  ESP-NOW and matched by `sizeof()` on receipt, **a width change means
  reflashing hub and satellite together** — flashing just one leaves the
  two sides disagreeing on message length, and everything using that
  message type silently stops parsing.
- **NVS/flash persistence via `Preferences`**: `SatelliteConfig` (per-
  satellite MAC, name, schedule, per-slot dose) is stored as one
  fixed-size binary blob under a single key (`prefs.putBytes`/
  `getBytes`), loaded at boot and compared by `sizeof()`. Simple and fine
  at this scale (a handful of satellites), but it means **any field
  added/removed/resized in that struct changes its `sizeof()` and fails
  the load-time size check**, silently resetting all satellite config to
  defaults on next boot — not a bug exactly, just a real, recurring cost
  of that storage choice worth remembering before casually adding a
  field. A per-field/versioned schema would avoid this if it becomes
  annoying enough to fix.
- **Live status without full page reloads**: the hub exposes a small
  `/status.json` (built from the same `computeSatStatus()`/
  `forEachHistoryEntry()` helpers the full-page render uses, so the two
  can't drift apart) that the page's own JS polls every 5s, patching only
  specific elements by id (status dot class, label text, last-seen,
  last-watered, history table innerHTML). Deliberately never touches the
  schedule-editing `<form>`s, so a poll can't discard an in-progress edit
  the way a naive full-page auto-refresh (e.g. `<meta http-equiv=refresh>`)
  would.
- **"Already watered today" guard must reset when its schedule changes,
  not just at midnight or reboot.** `lastWateredDayIndex[satellite][slot]`
  is keyed by slot *number*, not by the slot's configured time. Found
  2026-08-07: editing a slot's time-of-day after it had already fired
  earlier that day (e.g. while calibrating a schedule, moving a time
  around) left the guard still set from the old firing, silently
  suppressing the new time for the rest of the day — looked exactly like
  "the schedule randomly stops working," and a hub reboot "fixed" it only
  because reboot resets the whole guard array. Fixed by resetting
  `lastWateredDayIndex[idx][*]` inside `handleSatelliteSave()` — saving a
  schedule always makes every slot eligible again today. General lesson:
  any "did this already happen today" guard needs to be invalidated by
  *both* the day rolling over *and* the underlying condition changing,
  not just the former.
- **A self-triggered scheduled reboot needs an uptime floor, not just a
  "did this already happen today" flag.** The daily-reboot feature's
  guard (`lastRebootDayIndex`) is a plain global, not persisted — it
  resets to unset on *every* boot, including the reboot it just caused.
  Boot + WiFi reconnect + NTP resync can plausibly complete in under a
  minute, so without an explicit `millis() > 5 min` floor, a reboot that
  happens to land back in the same target minute would immediately
  trigger another one, forever. Any self-triggered periodic action needs
  this same guard.
- **Simulating network failure without router access**: renaming the
  router's SSID isn't always possible (ISP-locked, in this case). Added
  `/debug/wifi-disconnect` (calls `WiFi.disconnect()`) as a software-only
  way to force exactly the same `WiFi.status() != WL_CONNECTED` condition
  a real AP disappearance would cause — the hub can't tell the difference,
  which is the point. Note the HTTP response to that request itself will
  time out client-side (the disconnect happens mid-response), which is
  expected, not a failure.

## Secrets & deployment-specific config

`templates/` at the repo root holds committed blank templates for files
that are gitignored because they're machine/environment-specific (WiFi
credentials so far). See `templates/README.md` for the convention — a
template's path mirrors its real destination path, copy-then-edit to
(re)deploy. Extend this pattern for any new per-deployment file, not just
secrets.

`firmware/pump-satellite/include/config.h` (hub MAC + WiFi channel) is
*not* templated this way — it's not secret, just committed directly, since
it's shared config both v1 pump satellites will need identically.

## Design system (web UI)

`firmware/hub/include/web_ui.h` is the shared page shell + CSS design
tokens for every hub web page — see its own comments. Any new hub page
should call `pageShell(title, bodyHtml)` rather than writing its own
`<style>` block, so the whole site stays visually consistent. The page is
now a `.page` flex column of separate `.card` blocks (one per satellite,
plus a header card) rather than one big shared box — new sections should
follow that pattern, not add another giant card. Established sub-patterns
worth reusing rather than reinventing:
- `<details><summary>` styled as an actual button (not a text link) for
  anything that should default to collapsed (schedule editing, settings,
  history) — see the `details summary` rules.
- `.button-row` / `.action-row` for putting two of those side by side on
  narrow (phone) screens instead of stacking them.
- Elements that need live updates carry a stable `id="thing-<idx>"` (e.g.
  `dot-0`, `lastwater-0`, `history-0`) so the `/status.json` poll can find
  and patch them directly — see "Live status without full page reloads"
  above.
