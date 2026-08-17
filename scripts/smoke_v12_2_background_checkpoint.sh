#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v12_2-bgckpt.img}
MNT=${MNT:-/tmp/cryexts-v12_2-mnt}
INSPECT=${INSPECT:-/tmp/cryexts-v12_2-inspect.txt}

log_step() { echo "[v12.2] $1"; }
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
./mkfs.cryexts -f -G -X -A -I -T -M -Q -P 7 -L v122bgckpt "$IMG"
./cryextsck "$IMG"

log_step "mount and generate metadata transactions"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo python3 - "$MNT" <<'PY'
import os
import sys

root = sys.argv[1]
for shard in range(48):
    parent = os.path.join(root, "shard_%03d" % shard)
    os.mkdir(parent)
    for child in range(16):
        with open(os.path.join(parent, "entry_%03d" % child), "wb") as f:
            f.write(b"v12.2 background checkpoint payload")
PY
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

log_step "verify checkpoint flushed to idle"
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
assert sum(os.path.isdir(os.path.join(root, name)) for name in os.listdir(root)) == 48
for shard in range(48):
    path = os.path.join(root, "shard_%03d" % shard)
    assert len(os.listdir(path)) == 16
    for child in range(16):
        with open(os.path.join(path, "entry_%03d" % child), "rb") as f:
            assert f.read() == b"v12.2 background checkpoint payload"
PY
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
echo "v12.2 background checkpoint smoke test passed"
