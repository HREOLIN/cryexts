#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v3_3.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
KEY=${KEY:-v33-secret}
BAD_KEY=${BAD_KEY:-wrong-v33-secret}
PAYLOAD=${PAYLOAD:-/tmp/cryexts-v3_3.bin}
READBACK=${READBACK:-/tmp/cryexts-v3_3.out}
EXPECTED="hello cryexts v3.3 crypto api"

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
size = 131072
buf = bytearray(size)
for i in range(size):
    buf[i] = (i * 23 + 11) & 0xff
sys.stdout.buffer.write(buf)
PY

sudo cp "$PAYLOAD" "$MNT/dir1/big.bin"
sudo cat "$MNT/dir1/big.bin" > "$READBACK"
cmp "$PAYLOAD" "$READBACK"

sudo umount "$MNT"
./cryextsck "$IMG"

if grep -a -q "$EXPECTED" "$IMG"; then
	echo "v3.3 smoke test failed: plaintext found in image"
	exit 1
fi

if sudo mount -o "loop,key=$BAD_KEY" -t cryexts "$IMG" "$MNT"; then
	sudo umount "$MNT"
	echo "v3.3 smoke test failed: wrong key mounted successfully"
	exit 1
fi

sudo mount -o "loop,key=$KEY" -t cryexts "$IMG" "$MNT"
test "$(cat "$MNT/dir1/a.txt")" = "$EXPECTED"
sudo cat "$MNT/dir1/big.bin" > "$READBACK"
cmp "$PAYLOAD" "$READBACK"
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
trap - EXIT

echo "v3.3 crypto api smoke test passed"
