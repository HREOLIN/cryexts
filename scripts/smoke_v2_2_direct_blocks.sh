#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v2_2.img}
ENC_IMG=${ENC_IMG:-cryexts-v2_2-enc.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
KEY=${KEY:-v22-secret}
PAYLOAD=${PAYLOAD:-/tmp/cryexts-v2_2.bin}
READBACK=${READBACK:-/tmp/cryexts-v2_2.out}

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
rm -f "$IMG" "$ENC_IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f "$IMG"
./cryextsck "$IMG"

python3 - <<'PY' > "$PAYLOAD"
import sys
data = bytearray()
for i in range(32768):
    data.append((i * 13 + 7) & 0xff)
sys.stdout.buffer.write(data)
PY

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo cp "$PAYLOAD" "$MNT/big.bin"
sudo cat "$MNT/big.bin" > "$READBACK"
cmp "$PAYLOAD" "$READBACK"
stat --format='%s' "$READBACK" | grep -qx '32768'
sudo truncate -s 5000 "$MNT/big.bin"
test "$(stat --format='%s' "$MNT/big.bin")" = "5000"

sudo umount "$MNT"
./cryextsck "$IMG"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo cat "$MNT/big.bin" > "$READBACK"
cmp <(head -c 5000 "$PAYLOAD") "$READBACK"
sudo umount "$MNT"
sudo rmmod cryexts

dd if=/dev/zero of="$ENC_IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f "$ENC_IMG" -E "$KEY"
./cryextsck "$ENC_IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts -o "key=$KEY" "$ENC_IMG" "$MNT"
sudo cp "$PAYLOAD" "$MNT/secret.bin"
sudo cat "$MNT/secret.bin" > "$READBACK"
cmp "$PAYLOAD" "$READBACK"
sudo umount "$MNT"
./cryextsck "$ENC_IMG"
sudo mount -o loop -t cryexts -o "key=$KEY" "$ENC_IMG" "$MNT"
sudo cat "$MNT/secret.bin" > "$READBACK"
cmp "$PAYLOAD" "$READBACK"
sudo umount "$MNT"
sudo rmmod cryexts
trap - EXIT
cleanup

echo "v2.2 direct-block smoke test passed"
