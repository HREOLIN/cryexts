#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
EXPECTED="hello cryexts phase3"

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
./cryextsck "$IMG"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo mkdir "$MNT/dir1"
printf '%s\n' "$EXPECTED" | sudo tee "$MNT/dir1/a.txt" >/dev/null
test "$(cat "$MNT/dir1/a.txt")" = "$EXPECTED"
sudo umount "$MNT"
./cryextsck "$IMG"
sudo rmmod cryexts
trap - EXIT

echo "phase3 smoke test passed"
