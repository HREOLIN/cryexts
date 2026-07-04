#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v6_0.img}
SIZE_MB=${SIZE_MB:-128}

rm -f "$IMG" /tmp/cryexts-v6_0-journal.txt

make
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -J -P 7 -L v60jnl "$IMG"
./cryexts_journal_inspect "$IMG" | tee /tmp/cryexts-v6_0-journal.txt

grep -q '^version=6$' /tmp/cryexts-v6_0-journal.txt
grep -q '^journal_format=v2$' /tmp/cryexts-v6_0-journal.txt
grep -q '^control.block_type=1$' /tmp/cryexts-v6_0-journal.txt
grep -q '^descriptor.block_type=2$' /tmp/cryexts-v6_0-journal.txt
grep -q '^commit.block_type=3$' /tmp/cryexts-v6_0-journal.txt

python3 - <<'PY'
from pathlib import Path

values = {}
for line in Path("/tmp/cryexts-v6_0-journal.txt").read_text().splitlines():
    if "=" in line:
        k, v = line.split("=", 1)
        values[k] = v

required_equal = [
    ("control.checksum", "control.expected_checksum"),
    ("descriptor.checksum", "descriptor.expected_checksum"),
    ("commit.checksum", "commit.expected_checksum"),
]
for left, right in required_equal:
    if values.get(left) != values.get(right):
        raise SystemExit(f"checksum mismatch: {left} != {right}")

if values.get("control.active_sequence") != "0":
    raise SystemExit("fresh v6.0 journal should have active_sequence=0")
PY

./cryextsck "$IMG"
echo "v6.0 journal-layout smoke test passed"
