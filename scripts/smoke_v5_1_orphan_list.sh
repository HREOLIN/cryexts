#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v5_1.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-96}

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
./mkfs.cryexts -f -G -O -L v5orphan "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
printf 'v5.1-orphan-test\n' | sudo tee "$MNT/orphan.txt" >/dev/null
test "$(cat "$MNT/orphan.txt")" = "v5.1-orphan-test"
sudo rm -f "$MNT/orphan.txt"
test ! -e "$MNT/orphan.txt"
printf 'v5.1-recovery\n' | sudo tee "$MNT/recover.txt" >/dev/null
test "$(cat "$MNT/recover.txt")" = "v5.1-recovery"
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

./cryexts_orphan_inject "$IMG" recover.txt
echo "pre-replay orphan image injected"

if ./cryextsck "$IMG"; then
	echo "expected pre-replay cryextsck to fail on orphan recovery image" >&2
	exit 1
fi
echo "pre-replay fsck failed as expected"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
echo "mount-time orphan cleanup succeeded"
test ! -e "$MNT/recover.txt"
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"
echo "post-replay fsck is clean"

trap - EXIT
echo "v5.1 orphan-list smoke test passed"
