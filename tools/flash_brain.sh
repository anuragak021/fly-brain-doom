#!/usr/bin/env bash
# Flash the brain firmware to the Nucleo-H753ZI over the ST-Link with OpenOCD and run it.
# WARNING: erases the start of flash, which overwrites secure-boot's bootloader at 0x08000000.
# Usage: tools/flash_brain.sh [path/to/brain.bin]   (default: build/fw/brain.bin)
set -euo pipefail
BIN="${1:-build/fw/brain.bin}"
[ -f "$BIN" ] || { echo "no such file: $BIN (build first: see README)" >&2; exit 1; }
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "program ${BIN} 0x08000000 verify reset exit"
