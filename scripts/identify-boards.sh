#!/usr/bin/env bash
# Identifies which /dev/ttyUSB* port each physical board is on, by MAC
# address (a genuine per-chip hardware identifier) — not by port number
# and not by the USB-serial adapter's own serial string, which on the
# CP2102 clones used here is a non-unique factory default ("0001") shared
# by every board, so it can't tell them apart.
#
# Why this exists: Linux doesn't guarantee stable /dev/ttyUSB0/1
# numbering across reboots/replugs, and flashing the wrong firmware to
# the wrong physical board fails silently (everything compiles and runs,
# the boards just end up talking to themselves). See docs/firmware.md.
#
# Usage: scripts/identify-boards.sh

set -euo pipefail

ESPTOOL="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
PIO_PY=$(find "$HOME/.local/share/pipx/venvs/platformio" -name python3 -path "*/bin/*" | head -1)

# Known boards — add a line here as new satellites get built.
HUB_MAC="68:09:47:9e:8a:60"
declare -A KNOWN_BOARDS=(
  ["$HUB_MAC"]="hub (antenna board)"
  ["8c:94:df:4d:24:00"]="pump-satellite #1 (relay/pump board)"
  ["30:76:f5:92:44:f4"]="pump-satellite #2 (bare, no relay/pump wired yet)"
)

if [ -z "$PIO_PY" ]; then
  echo "Couldn't find PlatformIO's Python venv — is PlatformIO installed?" >&2
  exit 1
fi

shopt -s nullglob
ports=(/dev/ttyUSB*)
if [ ${#ports[@]} -eq 0 ]; then
  echo "No /dev/ttyUSB* devices found — is anything plugged in?"
  exit 0
fi

for port in "${ports[@]}"; do
  # `|| true` matters here: under `set -e -o pipefail`, a transient
  # esptool failure on *one* port (e.g. right after boards enumerate,
  # before the OS has the device fully ready) would otherwise abort the
  # whole script before checking the remaining ports. Seen in practice,
  # not hypothetical — a rerun moments later succeeded cleanly.
  mac=$("$PIO_PY" "$ESPTOOL" --port "$port" read_mac 2>/dev/null \
    | grep -oE "([0-9a-f]{2}:){5}[0-9a-f]{2}" | head -1 || true)
  if [ -z "$mac" ]; then
    echo "$port -> could not read (transient failure, or board busy/unplugged — try again)"
    continue
  fi
  label="${KNOWN_BOARDS[$mac]:-unknown board}"
  echo "$port -> $mac -> $label"
done
