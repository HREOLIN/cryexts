#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v3_0.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
SRC=${SRC:-/tmp/cryexts-v3_0-src.bin}
OUT=${OUT:-/tmp/cryexts-v3_0-out.bin}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$SRC" "$OUT"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f "$IMG"
./cryextsck "$IMG"

python3 - <<'PY' > "$SRC"
import sys
size = 1024 * 1024
buf = bytearray(size)
for i in range(size):
    buf[i] = (i * 31 + 7) & 0xff
sys.stdout.buffer.write(buf)
PY

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo cp "$SRC" "$MNT/large.bin"
sudo cp "$MNT/large.bin" "$OUT"
cmp "$SRC" "$OUT"

python3 - <<'PY'
with open("/tmp/cryexts-v3_0-src.bin", "rb") as f:
    f.seek(49152 - 32)
    data = f.read(64)
with open("/tmp/cryexts-v3_0-out.bin", "rb") as f:
    f.seek(49152 - 32)
    out = f.read(64)
assert data == out
PY

sudo truncate -s 60000 "$MNT/large.bin"
stat -c '%s' "$MNT/large.bin" | grep -qx '60000'
sudo cp "$MNT/large.bin" "$OUT"
cmp -n 60000 "$SRC" "$OUT"

sudo truncate -s 8192 "$MNT/large.bin"
stat -c '%s' "$MNT/large.bin" | grep -qx '8192'

sudo umount "$MNT"
./cryextsck "$IMG"

sudo mount -o loop -t cryexts "$IMG" "$MNT"
stat -c '%s' "$MNT/large.bin" | grep -qx '8192'
sudo cp "$MNT/large.bin" "$OUT"
cmp -n 8192 "$SRC" "$OUT"
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
trap - EXIT

echo "v3.0 single-indirect smoke test passed"
