#!/usr/bin/env bash
# Show the brain's live dashboard (STM32 ST-Link virtual COM port) in this terminal.
# The dashboard appears once the ESP32 is stepping the brain; see firmware/src/telemetry.h.
# Usage: tools/brain_monitor.sh [port]   (default: the ST-Link VCP found under /dev/serial/by-id)
# Needs a UTF-8 terminal at least 64 columns by 20 rows. Press Ctrl-C to quit.
set -euo pipefail
PORT="${1:-}"
if [ -z "$PORT" ]; then
  PORT="$(ls /dev/serial/by-id/*STLINK*-if02 2>/dev/null | head -n 1 || true)"
fi
[ -n "$PORT" ] && [ -e "$PORT" ] || { echo "no ST-Link port found (re-attach with usbipd?); pass one explicitly" >&2; exit 1; }
stty -F "$PORT" 115200 raw -echo -hupcl
trap 'printf "\033[?25h\033[0m\n"' EXIT
printf '\033[2J\033[H\033[2mwaiting for the ESP32 to step the brain on %s ...\033[0m' "$PORT"
cat "$PORT"
