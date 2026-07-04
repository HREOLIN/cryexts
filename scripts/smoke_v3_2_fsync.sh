#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v3_2.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
PAYLOAD=${PAYLOAD:-/tmp/cryexts-v3_2.bin}
OUT=${OUT:-/tmp/cryexts-v3_2.out}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$PAYLOAD" "$OUT"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f "$IMG"
./cryextsck "$IMG"

python3 - <<'PY' > "$PAYLOAD"
import sys
size = 131072
buf = bytearray(size)
for i in range(size):
    buf[i] = (i * 19 + 5) & 0xff
sys.stdout.buffer.write(buf)
PY

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo cp "$PAYLOAD" "$MNT/fsync.bin"
sync
sudo python3 - <<'PY'
with open("/tmp/cryexts-mnt/fsync.bin", "rb+") as f:
    f.flush()
    import os
    os.fsync(f.fileno())
PY

sudo cp "$MNT/fsync.bin" "$OUT"
cmp "$PAYLOAD" "$OUT"

sudo umount "$MNT"
./cryextsck "$IMG"

sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo cp "$MNT/fsync.bin" "$OUT"
cmp "$PAYLOAD" "$OUT"
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
trap - EXIT

echo "v3.2 fsync smoke test passed"
