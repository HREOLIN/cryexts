#!/usr/bin/env bash
set -euo pipefail

SCRIPT_TAG=${SCRIPT_TAG:-v10.1}
IMG=${IMG:-cryexts-v10_1.img}
MNT=${MNT:-/tmp/cryexts-v10_1-mnt}
LABEL=${LABEL:-v101cache}

TMP_A=/tmp/cryexts-v10_1-a.bin
TMP_B=/tmp/cryexts-v10_1-b.bin
OUT_1=/tmp/cryexts-v10_1-out1.bin
OUT_2=/tmp/cryexts-v10_1-out2.bin
OUT_3=/tmp/cryexts-v10_1-out3.bin

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
	sudo rm -f "$TMP_A" "$TMP_B" "$OUT_1" "$OUT_2" "$OUT_3"
}

trap cleanup EXIT

log_step "build cryexts"
make

log_step "prepare source patterns"
python3 - <<'PY'
from pathlib import Path
Path("/tmp/cryexts-v10_1-a.bin").write_bytes(b"A" * 4096 + b"B" * 4096 + b"C" * 4096)
Path("/tmp/cryexts-v10_1-b.bin").write_bytes(b"Z" * 4096)
PY

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

log_step "write initial file"
sudo dd if="$TMP_A" of="$MNT/cached.bin" bs=4K conv=fsync status=none

log_step "first cached read"
sudo dd if="$MNT/cached.bin" of="$OUT_1" bs=4K status=none
cmp -s "$TMP_A" "$OUT_1"

log_step "second cached read"
sudo dd if="$MNT/cached.bin" of="$OUT_2" bs=4K status=none
cmp -s "$TMP_A" "$OUT_2"

log_step "overwrite first page"
sudo dd if="$TMP_B" of="$MNT/cached.bin" bs=4K count=1 conv=notrunc status=none

log_step "read after overwrite"
sudo dd if="$MNT/cached.bin" of="$OUT_3" bs=4K status=none
python3 - <<'PY'
from pathlib import Path

old = Path("/tmp/cryexts-v10_1-a.bin").read_bytes()
new_prefix = Path("/tmp/cryexts-v10_1-b.bin").read_bytes()
got = Path("/tmp/cryexts-v10_1-out3.bin").read_bytes()

expected = new_prefix + old[len(new_prefix):]
if got != expected:
    raise SystemExit("cached read returned stale or corrupted data after overwrite")
PY

log_step "unmount"
sudo umount "$MNT"
log_step "remove module"
sudo rmmod cryexts

log_step "fsck after smoke"
./cryextsck "$IMG"

echo "v10.1 cached-read smoke test passed"
