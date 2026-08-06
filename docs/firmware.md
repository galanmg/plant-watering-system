# Firmware

Status (2026-08-06): hub and one pump satellite built and flashed, running
the real check-in/command/report protocol (not just a one-shot test), with
a measured flow rate (12.5mL/s) and WiFi/power-outage resilience. See
architecture.md's "Current implementation status" for what each one
actually does today.

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
  replugs.** After a reboot, assuming "port 0 is still the hub" led to
  flashing firmware onto the wrong physical boards — everything compiled
  and ran, the hub even booted and served its page fine, it just silently
  talked to itself instead of the satellite. Always confirm which port is
  which board physically before flashing after any reboot/replug — e.g.
  unplug one board and see which `/dev/ttyUSB*` disappears — rather than
  trusting the port number. (Hub = the board with the antenna; satellite =
  the board wired to the relay/pump.)
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
  (like a run report) 2-3x with a short gap. (3) Two `esp_now_send()`
  calls back-to-back in the same `loop()` iteration can result in one
  being dropped — if a blocking operation (like running a pump) can push
  a periodic send (like a check-in) to fire immediately afterward, reset
  that periodic timer explicitly rather than letting the two collide.

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
`<style>` block, so the whole site stays visually consistent.
