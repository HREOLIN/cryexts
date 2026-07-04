#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts.img}
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
grep -w cryexts /proc/filesystems
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
ls -la "$MNT"
sudo umount "$MNT"
sudo rmmod cryexts
trap - EXIT

echo "phase1 smoke test passed"
