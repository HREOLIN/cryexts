#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v12_3-ordered.img}
MNT=${MNT:-/tmp/cryexts-v12_3-mnt}
EXPECTED=${EXPECTED:-/tmp/cryexts-v12_3-expected.bin}
ACTUAL=${ACTUAL:-/tmp/cryexts-v12_3-actual.bin}
INSPECT=${INSPECT:-/tmp/cryexts-v12_3-journal.txt}

log_step() { echo "[v12.3] $1"; }

cleanup() {
	if mountpoint -q "$MNT"; then sudo umount "$MNT" || true; fi
	if lsmod | grep -q '^cryexts '; then sudo rmmod cryexts || true; fi
	sudo rm -f "$EXPECTED" "$ACTUAL" "$INSPECT"
}
trap cleanup EXIT

log_step "build"
make
log_step "mkfs ring image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=128 status=none
./mkfs.cryexts -f -G -X -A -I -T -M -Q -P 7 -L v123ordered "$IMG"
./cryextsck "$IMG"

log_step "mount and write ordered data"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo python3 - "$MNT/ordered.bin" "$EXPECTED" <<'PY'
import os
import sys

path, expected = sys.argv[1:]
data = b"A" * 4096 + b"B" * 4096 + b"C" * 4096 + b"D" * 4096
with open(path, "wb", buffering=0) as f:
    f.write(data)
    os.fsync(f.fileno())
with open(path, "r+b", buffering=0) as f:
    f.seek(4096)
    f.write(b"Z" * 4096)
    os.fsync(f.fileno())
with open(expected, "wb") as f:
    f.write(data[:4096] + b"Z" * 4096 + data[8192:])
PY

log_step "metadata update and remount"
sudo mkdir "$MNT/checkpointed"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"
./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^journal_ring=1$' "$INSPECT"
grep -q '^control.idle=1$' "$INSPECT"
HEAD=$(awk -F= '$1 == "control.ring_head" { print $2 }' "$INSPECT")
TAIL=$(awk -F= '$1 == "control.ring_tail" { print $2 }' "$INSPECT")
test -n "$HEAD" && test "$HEAD" = "$TAIL"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo dd if="$MNT/ordered.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"
test -d "$MNT/checkpointed"
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
echo "v12.3 ordered durability smoke test passed"
