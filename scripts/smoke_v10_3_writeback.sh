#!/usr/bin/env bash
set -euo pipefail

SCRIPT_TAG=${SCRIPT_TAG:-v10.3}
IMG=${IMG:-cryexts-v10_3.img}
MNT=${MNT:-/tmp/cryexts-v10_3-mnt}
LABEL=${LABEL:-v103writeback}

EXPECTED=/tmp/cryexts-v10_3-expected.bin
SYNC_EXPECTED=/tmp/cryexts-v10_3-sync-expected.bin
ACTUAL=/tmp/cryexts-v10_3-actual.bin

log_step() {
	echo "[$SCRIPT_TAG] $1"
}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	sudo rm -f "$EXPECTED" "$SYNC_EXPECTED" "$ACTUAL"
}

trap cleanup EXIT

log_step "build cryexts"
make

log_step "prepare image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=128 status=none

log_step "mkfs"
./mkfs.cryexts -f -G -X -A -I -T -M -J -P 7 -L "$LABEL" "$IMG"

log_step "fsck after mkfs"
./cryextsck "$IMG"

log_step "insert module"
sudo insmod cryexts.ko
log_step "prepare mountpoint"
sudo mkdir -p "$MNT"
log_step "mount cryexts"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

log_step "dirty small writes"
python3 - "$EXPECTED" <<'PY'
from pathlib import Path
import sys

data = b"".join(bytes([i % 251]) * 512 for i in range(256))
Path(sys.argv[1]).write_bytes(data)
PY
sudo python3 - "$MNT/writeback.bin" <<'PY'
import sys

with open(sys.argv[1], "wb", buffering=0) as f:
    for i in range(256):
        f.write(bytes([i % 251]) * 512)
PY

log_step "read dirty pages before fsync"
sudo dd if="$MNT/writeback.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"

log_step "fsync dirty file"
sudo python3 - "$MNT/writeback.bin" <<'PY'
import os
import sys

with open(sys.argv[1], "rb") as f:
    os.fsync(f.fileno())
PY

log_step "system sync writeback"
python3 - "$SYNC_EXPECTED" <<'PY'
from pathlib import Path
import sys

Path(sys.argv[1]).write_bytes(b"sync-v10.3" * 4096)
PY
sudo python3 - "$MNT/sync.bin" <<'PY'
import sys

with open(sys.argv[1], "wb", buffering=0) as f:
    f.write(b"sync-v10.3" * 4096)
PY
sync

log_step "unlink dirty file"
sudo python3 - "$MNT/delete-me.bin" <<'PY'
import sys

with open(sys.argv[1], "wb", buffering=0) as f:
    f.write(b"delete-me" * 4096)
PY
sudo rm "$MNT/delete-me.bin"

log_step "remount"
sudo umount "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

log_step "verify fsync file after remount"
sudo dd if="$MNT/writeback.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"

log_step "verify sync file after remount"
sudo dd if="$MNT/sync.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$SYNC_EXPECTED" "$ACTUAL"
test ! -e "$MNT/delete-me.bin"

log_step "unmount"
sudo umount "$MNT"
log_step "remove module"
sudo rmmod cryexts

log_step "fsck after smoke"
./cryextsck "$IMG"

echo "v10.3 writeback smoke test passed"
