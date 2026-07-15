#!/usr/bin/env bash
set -euo pipefail

SCRIPT_TAG=${SCRIPT_TAG:-v10.2}
IMG=${IMG:-cryexts-v10_2.img}
MNT=${MNT:-/tmp/cryexts-v10_2-mnt}
LABEL=${LABEL:-v102write}

EXPECTED=/tmp/cryexts-v10_2-expected.bin
ACTUAL=/tmp/cryexts-v10_2-actual.bin

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
	sudo rm -f "$EXPECTED" "$ACTUAL"
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

log_step "small buffered writes"
python3 - "$EXPECTED" <<'PY'
from pathlib import Path
import sys

data = b"".join(bytes([65 + i % 26]) * 512 for i in range(96))
Path(sys.argv[1]).write_bytes(data)
PY
sudo python3 - "$MNT/buffered.bin" <<'PY'
import os
import sys

with open(sys.argv[1], "wb", buffering=0) as f:
    for i in range(96):
        f.write(bytes([65 + i % 26]) * 512)
    os.fsync(f.fileno())
PY

log_step "first read and compare"
sudo dd if="$MNT/buffered.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"

log_step "partial overwrite after cached read"
python3 - "$EXPECTED" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
data[1234:1934] = b"Z" * 700
path.write_bytes(data)
PY
sudo python3 - "$MNT/buffered.bin" <<'PY'
import os
import sys

with open(sys.argv[1], "r+b", buffering=0) as f:
    f.seek(1234)
    f.write(b"Z" * 700)
    os.fsync(f.fileno())
PY

log_step "append"
python3 - "$EXPECTED" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
path.write_bytes(path.read_bytes() + b"tail-v10.2" * 37)
PY
sudo python3 - "$MNT/buffered.bin" <<'PY'
import os
import sys

with open(sys.argv[1], "ab", buffering=0) as f:
    f.write(b"tail-v10.2" * 37)
    os.fsync(f.fileno())
PY

log_step "truncate"
truncate -s 18000 "$EXPECTED"
sudo truncate -s 18000 "$MNT/buffered.bin"

log_step "compare before remount"
sudo dd if="$MNT/buffered.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"

log_step "remount"
sudo umount "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

log_step "compare after remount"
sudo dd if="$MNT/buffered.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"

log_step "unmount"
sudo umount "$MNT"
log_step "remove module"
sudo rmmod cryexts

log_step "fsck after smoke"
./cryextsck "$IMG"

echo "v10.2 buffered-write smoke test passed"
