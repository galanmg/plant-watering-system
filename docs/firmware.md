# Firmware

Status (2026-08-04): hub and one pump satellite built and flashed. See
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

## Secrets & deployment-specific config

`templates/` at the repo root holds committed blank templates for files
that are gitignored because they're machine/environment-specific (WiFi
credentials so far). See `templates/README.md` for the convention — a
template's path mirrors its real destination path, copy-then-edit to
(re)deploy. Extend this pattern for any new per-deployment file, not just
secrets.

`firmware/pump-satellite/include/config.h` (hub MAC + WiFi channel) is
*not* templated this way — it's not secret, just committed directly, since
it's shared config all 3 pump satellites will need identically.

## Design system (web UI)

`firmware/hub/include/web_ui.h` is the shared page shell + CSS design
tokens for every hub web page — see its own comments. Any new hub page
should call `pageShell(title, bodyHtml)` rather than writing its own
`<style>` block, so the whole site stays visually consistent.
