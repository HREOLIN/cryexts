#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v12_0-ring.img}
MNT=${MNT:-/tmp/cryexts-v12_0-mnt}
INSPECT=${INSPECT:-/tmp/cryexts-v12_0-inspect.txt}
EXPECTED=${EXPECTED:-/tmp/cryexts-v12_0-expected.txt}

log_step() {
	echo "[v12.0] $1"
}

cleanup() {
	if mountpoint -q "$MNT"; then sudo umount "$MNT" || true; fi
	if lsmod | grep -q '^cryexts '; then sudo rmmod cryexts || true; fi
	sudo rm -f "$INSPECT" "$EXPECTED"
}
trap cleanup EXIT

log_step "build"
make

log_step "mkfs ring journal image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=128 status=none
./mkfs.cryexts -f -G -X -A -I -T -M -Q -P 7 -L v120ring "$IMG"
./cryextsck "$IMG"

log_step "inspect initialized ring"
./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^journal_format=v3$' "$INSPECT"
grep -q '^journal_ring=1$' "$INSPECT"
grep -q '^control.ring_valid=1$' "$INSPECT"
RING_START=$(awk -F= '$1 == "control.ring_start" { print $2 }' "$INSPECT")
RING_HEAD=$(awk -F= '$1 == "control.ring_head" { print $2 }' "$INSPECT")
RING_TAIL=$(awk -F= '$1 == "control.ring_tail" { print $2 }' "$INSPECT")
test -n "$RING_START"
test "$RING_HEAD" = "$RING_START"
test "$RING_TAIL" = "$RING_START"

log_step "mount and commit metadata"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
printf '%s\n' 'v12 ring journal payload' >"$EXPECTED"
sudo cp "$EXPECTED" "$MNT/ring.txt"
sudo python3 - "$MNT/ring.txt" <<'PY'
import os
import sys

with open(sys.argv[1], "rb") as stream:
    os.fsync(stream.fileno())
PY
sudo umount "$MNT"
sudo rmmod cryexts

log_step "verify checkpointed ring"
./cryextsck "$IMG"
./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^journal_ring=1$' "$INSPECT"
grep -q '^control.ring_valid=1$' "$INSPECT"
grep -q '^control.idle=1$' "$INSPECT"
grep -q '^control.checkpoint_complete=1$' "$INSPECT"

log_step "remount and verify data"
sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
cmp -s "$EXPECTED" "$MNT/ring.txt"
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
echo "v12.0 journal ring layout smoke test passed"
