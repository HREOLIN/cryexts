#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v2_5.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
KEY=${KEY:-v25-secret}
BAD_KEY=${BAD_KEY:-wrong-v25-secret}
PAYLOAD=${PAYLOAD:-/tmp/cryexts-v2_5.bin}
READBACK=${READBACK:-/tmp/cryexts-v2_5.out}
EXPECTED="hello cryexts v2.5 encryption"

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$PAYLOAD" "$READBACK"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -E "$KEY" "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o "loop,key=$KEY" -t cryexts "$IMG" "$MNT"

sudo mkdir -p "$MNT/dir1"
printf '%s\n' "$EXPECTED" | sudo tee "$MNT/dir1/a.txt" >/dev/null
test "$(cat "$MNT/dir1/a.txt")" = "$EXPECTED"

python3 - <<'PY' > "$PAYLOAD"
import sys
data = bytearray()
for i in range(32768):
    data.append((i * 17 + 9) & 0xff)
sys.stdout.buffer.write(data)
PY

sudo cp "$PAYLOAD" "$MNT/dir1/big.bin"
sudo cat "$MNT/dir1/big.bin" > "$READBACK"
cmp "$PAYLOAD" "$READBACK"

sudo umount "$MNT"
./cryextsck "$IMG"

if grep -a -q "$EXPECTED" "$IMG"; then
	echo "v2.5 smoke test failed: plaintext found in image"
	exit 1
fi

if sudo mount -o "loop,key=$BAD_KEY" -t cryexts "$IMG" "$MNT"; then
	sudo umount "$MNT"
	echo "v2.5 smoke test failed: wrong key mounted successfully"
	exit 1
fi

sudo mount -o "loop,key=$KEY" -t cryexts "$IMG" "$MNT"
test "$(cat "$MNT/dir1/a.txt")" = "$EXPECTED"
sudo cat "$MNT/dir1/big.bin" > "$READBACK"
cmp "$PAYLOAD" "$READBACK"
sudo umount "$MNT"
sudo rmmod cryexts
trap - EXIT

echo "v2.5 encryption smoke test passed"
