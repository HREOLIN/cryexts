#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
KEY=${KEY:-cryexts-phase4-key}
BAD_KEY=${BAD_KEY:-wrong-cryexts-key}
EXPECTED="hello cryexts encrypted phase4"

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
./mkfs.cryexts -f -E "$KEY" "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o "loop,key=$KEY" -t cryexts "$IMG" "$MNT"

sudo mkdir "$MNT/dir1"
printf '%s\n' "$EXPECTED" | sudo tee "$MNT/dir1/a.txt" >/dev/null
test "$(cat "$MNT/dir1/a.txt")" = "$EXPECTED"
sudo umount "$MNT"

./cryextsck "$IMG"
if grep -a -q "$EXPECTED" "$IMG"; then
	echo "phase4 smoke test failed: plaintext was found in image"
	exit 1
fi

if sudo mount -o "loop,key=$BAD_KEY" -t cryexts "$IMG" "$MNT"; then
	sudo umount "$MNT"
	echo "phase4 smoke test failed: wrong key mounted successfully"
	exit 1
fi

sudo mount -o "loop,key=$KEY" -t cryexts "$IMG" "$MNT"
test "$(cat "$MNT/dir1/a.txt")" = "$EXPECTED"
sudo umount "$MNT"
sudo rmmod cryexts
trap - EXIT

echo "phase4 smoke test passed"
