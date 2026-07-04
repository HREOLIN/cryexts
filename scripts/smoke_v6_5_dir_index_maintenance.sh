#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v6_5.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
TARGET_DIR=${TARGET_DIR:-$MNT/idx}
INSPECT_OUT=${INSPECT_OUT:-/tmp/cryexts-v6_5-dir-index.txt}
FILE_COUNT=${FILE_COUNT:-320}
DELETE_COUNT=${DELETE_COUNT:-40}
RENAME_COUNT=${RENAME_COUNT:-30}
LINK_COUNT=${LINK_COUNT:-10}

name_for() {
	printf 'file_%05d_abcdefghijklmnopqrstuvwxyz0123456789' "$1"
}

renamed_for() {
	printf 'renamed_%05d_abcdefghijklmnopqrstuvwxyz0123456789' "$1"
}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$INSPECT_OUT"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -J -P 7 -L v65diridx "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$TARGET_DIR"

for i in $(seq 1 "$FILE_COUNT"); do
	name=$(name_for "$i")
	printf 'v6.5 dir-index entry %s\n' "$i" | sudo tee "$TARGET_DIR/$name" >/dev/null
done

for i in $(seq 1 "$DELETE_COUNT"); do
	name=$(name_for "$i")
	sudo rm -f "$TARGET_DIR/$name"
done

rename_start=$((DELETE_COUNT + 1))
rename_end=$((DELETE_COUNT + RENAME_COUNT))
for i in $(seq "$rename_start" "$rename_end"); do
	old_name=$(name_for "$i")
	new_name=$(renamed_for "$i")
	sudo mv "$TARGET_DIR/$old_name" "$TARGET_DIR/$new_name"
done

link_start=$((rename_end + 1))
link_end=$((rename_end + LINK_COUNT))
for i in $(seq "$link_start" "$link_end"); do
	src=$(name_for "$i")
	link_name=$(printf 'hardlink_%05d_abcdefghijklmnopqrstuvwxyz0123456789' "$i")
	sudo ln "$TARGET_DIR/$src" "$TARGET_DIR/$link_name"
done

test -f "$TARGET_DIR/$(renamed_for "$rename_start")"
test ! -e "$TARGET_DIR/$(name_for 1)"
test -f "$TARGET_DIR/$(name_for "$FILE_COUNT")"
dir_ino=$(stat -c %i "$TARGET_DIR")
expected_entries=$((2 + FILE_COUNT - DELETE_COUNT + LINK_COUNT))

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_dir_index_inspect "$IMG" "$dir_ino" | tee "$INSPECT_OUT"

python3 - "$INSPECT_OUT" "$expected_entries" <<'PY'
import sys
from pathlib import Path

path = Path(sys.argv[1])
expected_entries = int(sys.argv[2])
values = {}
multi_block_bucket = False

for line in path.read_text().splitlines():
    if "=" not in line:
        continue
    key, value = line.split("=", 1)
    if key.startswith("bucket["):
        mask = int(value, 16)
        if mask & (mask - 1):
            multi_block_bucket = True
        continue
    values[key] = int(value)

entries = values.get("entries", 0)
dir_blocks = values.get("dir_blocks", 0)
active_buckets = values.get("active_buckets", 0)
mask_refs = values.get("mask_refs", 0)

if entries != expected_entries:
    raise SystemExit(f"entries {entries} != expected {expected_entries}")
if dir_blocks < 2:
    raise SystemExit(f"directory did not grow enough: dir_blocks={dir_blocks}")
if active_buckets < 32:
    raise SystemExit(f"too few active buckets: {active_buckets}")
if mask_refs < active_buckets:
    raise SystemExit(f"mask refs {mask_refs} < active buckets {active_buckets}")
if not multi_block_bucket:
    raise SystemExit("no bucket spans more than one directory block")

print(f"entries={entries}")
print(f"dir_blocks={dir_blocks}")
print(f"active_buckets={active_buckets}")
print(f"mask_refs={mask_refs}")
print("multi_block_bucket=1")
PY

./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
test -f "$TARGET_DIR/$(renamed_for "$rename_start")"
test ! -e "$TARGET_DIR/$(name_for 1)"
test -f "$TARGET_DIR/$(name_for "$FILE_COUNT")"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

trap - EXIT
echo "v6.5 directory-index maintenance smoke test passed"
