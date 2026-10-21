"""Write an IWAD to the ESP32-S3 'wad' partition, after checking the file and the board.

Checks, in order, and stops at the first failure:
  1. The file is an IWAD whose directory and lumps lie inside the file (same rules as
     components/doom_platform/wad_check.c), and it fits the partition in partitions.csv.
  2. The serial port belongs to the expected USB device (serial number), so a Nucleo or
     another board plugged into the same laptop is never written.
Then it runs ESP-IDF's parttool.py write_partition.

Run from the repository root inside the ESP-IDF environment:
    . ~/esp/esp-idf/export.sh
    python tools/flash_wad.py --port /dev/ttyACM0 --serial 5B14083969 --wad /path/to/doom1.wad
Add --dry-run to perform every check without writing.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import struct
import subprocess
import sys
from pathlib import Path

PARTITION_NAME = "wad"
HEADER_BYTES = 12
DIR_ENTRY_BYTES = 16
MAX_LUMPS = 65536
# Known good shareware DOOM1.WAD v1.9 (id Software), for an informative message only.
KNOWN_MD5 = {"f0cefca49926d00903cf57551d901abe": "DOOM1.WAD shareware v1.9"}


class CheckError(RuntimeError):
    """A pre-flash check failed; nothing was written."""


def check_wad(data: bytes) -> tuple[int, int]:
    """Return (num_lumps, used_bytes) or raise CheckError."""
    if len(data) < HEADER_BYTES:
        raise CheckError("file is smaller than a WAD header")
    magic, num, dir_ofs = struct.unpack_from("<4sii", data, 0)
    if magic != b"IWAD":
        raise CheckError(f"not an IWAD (magic {magic!r})")
    if not 0 <= num <= MAX_LUMPS:
        raise CheckError(f"bad lump count {num}")
    dir_end = dir_ofs + num * DIR_ENTRY_BYTES
    if dir_ofs < 0 or dir_end > len(data):
        raise CheckError("directory lies outside the file")
    used = dir_end
    for i in range(num):
        pos, size = struct.unpack_from("<ii", data, dir_ofs + i * DIR_ENTRY_BYTES)
        if pos < 0 or size < 0 or pos + size > len(data):
            raise CheckError(f"lump {i} lies outside the file")
        used = max(used, pos + size)
    return num, used


def partition_size(csv_path: Path, name: str) -> int:
    with csv_path.open(newline="") as f:
        for row in csv.reader(line for line in f if not line.lstrip().startswith("#")):
            cells = [c.strip() for c in row]
            if len(cells) >= 5 and cells[0] == name:
                return int(cells[4], 0)
    raise CheckError(f"no partition named '{name}' in {csv_path}")


def port_serial(port: str) -> str | None:
    from serial.tools import list_ports  # pyserial ships with the ESP-IDF environment

    real = os.path.realpath(port)
    for p in list_ports.comports():
        if os.path.realpath(p.device) == real:
            return p.serial_number
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True, help="serial port of the ESP32-S3")
    parser.add_argument(
        "--serial",
        required=True,
        help="expected USB serial number of that port (udevadm)",
    )
    parser.add_argument(
        "--wad", type=Path, required=True, help="IWAD file, e.g. doom1.wad"
    )
    parser.add_argument("--partitions", type=Path, default=Path("partitions.csv"))
    parser.add_argument(
        "--dry-run", action="store_true", help="check only, write nothing"
    )
    args = parser.parse_args()

    try:
        data = args.wad.read_bytes()
        num, used = check_wad(data)
        md5 = hashlib.md5(data).hexdigest()  # identification only, not security
        print(f"WAD ok: {args.wad} {len(data)} bytes, {num} lumps, md5 {md5}", end="")
        print(f" ({KNOWN_MD5[md5]})" if md5 in KNOWN_MD5 else " (unknown version)")
        if used != len(data):
            print(f"note: {len(data) - used} trailing bytes after the last lump")
        size = partition_size(args.partitions, PARTITION_NAME)
        if len(data) > size:
            raise CheckError(
                f"WAD is {len(data)} bytes, partition '{PARTITION_NAME}' is {size}"
            )
        print(f"fits partition '{PARTITION_NAME}' ({size} bytes)")
        found = port_serial(args.port)
        if found != args.serial:
            raise CheckError(
                f"{args.port} has USB serial {found!r}, expected {args.serial!r}. Not writing."
            )
        print(f"port ok: {args.port} serial {found}")
    except (OSError, CheckError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    idf = os.environ.get("IDF_PATH")
    if not idf and not args.dry_run:
        print(
            "error: IDF_PATH not set; run '. ~/esp/esp-idf/export.sh' first",
            file=sys.stderr,
        )
        return 2
    idf = idf or "$IDF_PATH"
    cmd = [
        sys.executable,
        str(Path(idf) / "components" / "partition_table" / "parttool.py"),
        "--port",
        args.port,
        "--partition-table-file",
        str(args.partitions),
        "write_partition",
        "--partition-name",
        PARTITION_NAME,
        "--input",
        str(args.wad),
    ]
    print(("dry run, would run: " if args.dry_run else "running: ") + " ".join(cmd))
    if args.dry_run:
        return 0
    return subprocess.run(cmd, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
