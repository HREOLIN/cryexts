#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v12_4-stability.img}
MNT=${MNT:-/tmp/cryexts-v12_4-mnt}
INSPECT=${INSPECT:-/tmp/cryexts-v12_4-journal.txt}
COUNT=${COUNT:-96}

log_step() { echo "[v12.4] $1"; }

cleanup() {
	if mountpoint -q "$MNT"; then sudo umount "$MNT" || true; fi
	if lsmod | grep -q '^cryexts '; then sudo rmmod cryexts || true; fi
	sudo rm -f "$INSPECT"
}
trap cleanup EXIT

test "$COUNT" -gt 0
test "$COUNT" -le 160

log_step "build"
make
log_step "mkfs ring image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=128 status=none
./mkfs.cryexts -f -G -X -A -I -T -M -Q -P 7 -L v124stable "$IMG"
./cryextsck "$IMG"

log_step "first mount: create, overwrite, rename, and delete"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo python3 - "$MNT" "$COUNT" <<'PY'
import os
import sys

root = sys.argv[1]
count = int(sys.argv[2])
for shard in range(2):
    os.mkdir(os.path.join(root, "shard_%d" % shard))

for i in range(count):
    parent = os.path.join(root, "shard_%d" % (i % 2))
    path = os.path.join(parent, "item_%03d.tmp" % i)
    data = ("item=%03d|" % i).encode() * 700
    with open(path, "wb", buffering=0) as f:
        f.write(data)
        os.fsync(f.fileno())
    with open(path, "r+b", buffering=0) as f:
        f.seek(4096)
        f.write(("updated=%03d|" % i).encode() * 128)
        os.fsync(f.fileno())
    final = path[:-4] + ".bin"
    os.rename(path, final)
    if i % 5 == 0:
        os.unlink(final)
PY
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

log_step "first recovery check"
./cryextsck "$IMG"

log_step "second mount: verify contents and add another metadata round"
sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo python3 - "$MNT" "$COUNT" <<'PY'
import os
import sys

root = sys.argv[1]
count = int(sys.argv[2])
for i in range(count):
    parent = os.path.join(root, "shard_%d" % (i % 2))
    path = os.path.join(parent, "item_%03d.bin" % i)
    if i % 5 == 0:
        assert not os.path.exists(path)
        continue
    with open(path, "rb") as f:
        data = f.read()
    assert data.startswith(("item=%03d|" % i).encode())
    assert data[4096:].startswith(("updated=%03d|" % i).encode())
    os.rename(path, path[:-4] + ".verified")
PY
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

log_step "ring cleanup and final fsck"
./cryextsck "$IMG"
./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^journal_ring=1$' "$INSPECT"
grep -q '^control.idle=1$' "$INSPECT"
grep -q '^control.checkpoint_complete=1$' "$INSPECT"
HEAD=$(awk -F= '$1 == "control.ring_head" { print $2 }' "$INSPECT")
TAIL=$(awk -F= '$1 == "control.ring_tail" { print $2 }' "$INSPECT")
test -n "$HEAD" && test "$HEAD" = "$TAIL"

echo "v12.4 stability smoke test passed (COUNT=$COUNT)"
