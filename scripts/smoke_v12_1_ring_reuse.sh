#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v12_1-ring.img}
MNT=${MNT:-/tmp/cryexts-v12_1-mnt}
INSPECT=${INSPECT:-/tmp/cryexts-v12_1-inspect.txt}

log_step() { echo "[v12.1] $1"; }
cleanup() {
    if mountpoint -q "$MNT"; then sudo umount "$MNT" || true; fi
    if lsmod | grep -q '^cryexts '; then sudo rmmod cryexts || true; fi
    sudo rm -rf "$MNT" "$INSPECT"
}
trap cleanup EXIT

log_step "build"
make
log_step "mkfs ring image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=128 status=none
./mkfs.cryexts -f -G -X -A -I -T -M -Q -P 7 -L v121ring "$IMG"
./cryextsck "$IMG"

log_step "mount and generate reusable transactions"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo python3 - "$MNT" <<'PY'
import os
import sys

root = sys.argv[1]
for shard in range(24):
    parent = os.path.join(root, "shard_%03d" % shard)
    os.mkdir(parent)
    for child in range(8):
        os.mkdir(os.path.join(parent, "entry_%03d" % child))
PY
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

log_step "verify checkpoint and ring reuse"
./cryextsck "$IMG"
./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^journal_ring=1$' "$INSPECT"
grep -q '^control.ring_valid=1$' "$INSPECT"
grep -q '^control.idle=1$' "$INSPECT"
grep -q '^control.checkpoint_complete=1$' "$INSPECT"
HEAD=$(awk -F= '$1 == "control.ring_head" { print $2 }' "$INSPECT")
TAIL=$(awk -F= '$1 == "control.ring_tail" { print $2 }' "$INSPECT")
test -n "$HEAD" && test "$HEAD" = "$TAIL"

log_step "remount and verify directory tree"
sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo python3 - "$MNT" <<'PY'
import os
import sys

root = sys.argv[1]
assert sum(os.path.isdir(os.path.join(root, name)) for name in os.listdir(root)) == 24
for shard in range(24):
    path = os.path.join(root, "shard_%03d" % shard)
    assert len(os.listdir(path)) == 8
PY
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
echo "v12.1 journal ring reuse smoke test passed"
