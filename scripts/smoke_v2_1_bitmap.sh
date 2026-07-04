#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v2_1.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
EXPECTED1="hello cryexts v2.1 first"
EXPECTED2="hello cryexts v2.1 second"

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

printf '%s\n' "$EXPECTED1" | sudo tee "$MNT/a.txt" >/dev/null
test "$(cat "$MNT/a.txt")" = "$EXPECTED1"
sudo rm "$MNT/a.txt"
sudo touch "$MNT/b.txt"
printf '%s\n' "$EXPECTED2" | sudo tee "$MNT/b.txt" >/dev/null
test "$(cat "$MNT/b.txt")" = "$EXPECTED2"

sudo mkdir "$MNT/dir1"
sudo rmdir "$MNT/dir1"
sudo mkdir "$MNT/dir2"
sudo touch "$MNT/dir2/c.txt"
printf '%s\n' "$EXPECTED2" | sudo tee "$MNT/dir2/c.txt" >/dev/null
test "$(cat "$MNT/dir2/c.txt")" = "$EXPECTED2"

sudo umount "$MNT"
./cryextsck "$IMG"

sudo mount -o loop -t cryexts "$IMG" "$MNT"
test "$(cat "$MNT/b.txt")" = "$EXPECTED2"
test "$(cat "$MNT/dir2/c.txt")" = "$EXPECTED2"
sudo umount "$MNT"
sudo rmmod cryexts
trap - EXIT

echo "v2.1 bitmap smoke test passed"
