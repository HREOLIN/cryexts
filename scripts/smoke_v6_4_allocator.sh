#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v6_4.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
SRC=${SRC:-/tmp/cryexts-v6_4-src.bin}
TARGET_DIR=${TARGET_DIR:-$MNT/alloc}
SEQ_FILE=${SEQ_FILE:-$TARGET_DIR/seq-reserved.bin}
INSPECT_OUT=${INSPECT_OUT:-/tmp/cryexts-v6_4-alloc-inspect.txt}
TEST_BLOCKS=${TEST_BLOCKS:-96}
SIB_COUNT=${SIB_COUNT:-12}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$SRC" "$INSPECT_OUT"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -J -P 7 -L v64alloc "$IMG"
./cryextsck "$IMG"

dd if=/dev/urandom of="$SRC" bs=4096 count="$TEST_BLOCKS" status=none

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$TARGET_DIR"

sudo cp "$SRC" "$SEQ_FILE"
cmp "$SRC" "$SEQ_FILE"
seq_inode=$(stat -c %i "$SEQ_FILE")

sibling_inodes=()
for i in $(seq 1 "$SIB_COUNT"); do
	name="$TARGET_DIR/sibling_$i.txt"
	printf 'v6.4 sibling %s\n' "$i" | sudo tee "$name" >/dev/null
	sibling_inodes+=("$(stat -c %i "$name")")
done

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_alloc_inspect "$IMG" "$seq_inode" "${sibling_inodes[@]}" | tee "$INSPECT_OUT"

python3 - "$INSPECT_OUT" "$seq_inode" "${sibling_inodes[@]}" <<'PY'
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
seq_inode = sys.argv[2]
siblings = sys.argv[3:]
values = {}

for line in path.read_text().splitlines():
    if "=" not in line:
        continue
    key, value = line.split("=", 1)
    values[key] = int(value)

seq_group = values[f"inode[{seq_inode}].group"]
seq_data_group = values[f"inode[{seq_inode}].first_data_group"]
largest = values[f"inode[{seq_inode}].largest_extent_len"]

if seq_data_group != seq_group:
    raise SystemExit(
        f"seq file data group {seq_data_group} != inode group {seq_group}")
if largest < 64:
    raise SystemExit(f"largest extent too small for reservation MVP: {largest}")

sibling_groups = {values[f"inode[{ino}].group"] for ino in siblings}
if len(sibling_groups) != 1:
    raise SystemExit(f"sibling inode groups are not local: {sorted(sibling_groups)}")
if next(iter(sibling_groups)) != seq_group:
    raise SystemExit(
        f"sibling inode group {next(iter(sibling_groups))} != parent locality group {seq_group}")

print(f"seq_inode_group={seq_group}")
print(f"seq_data_group={seq_data_group}")
print(f"largest_extent_len={largest}")
print(f"sibling_inode_groups={sorted(sibling_groups)}")
PY

./cryextsck "$IMG"

trap - EXIT
echo "v6.4 allocator locality smoke test passed"
