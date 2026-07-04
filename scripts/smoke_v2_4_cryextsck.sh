#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v2_4.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo touch "$MNT/a.txt"
printf '%s\n' "hello cryexts v2.4" | sudo tee "$MNT/a.txt" >/dev/null
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"
python3 - <<'PY' "$IMG"
import sys
from pathlib import Path

img = Path(sys.argv[1])
data = bytearray(img.read_bytes())
data[4096 + 0] &= ~0x01
data[4096 * 2 + 0] &= ~0x01
img.write_bytes(data)
PY

if ./cryextsck "$IMG"; then
	echo "expected cryextsck to fail on corrupted bitmap"
	exit 1
fi

./cryextsck --repair "$IMG"
./cryextsck "$IMG"

echo "v2.4 cryextsck smoke test passed"
