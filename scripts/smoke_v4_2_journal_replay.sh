#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v4_2.img}
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
./mkfs.cryexts -f -G -L v4journal "$IMG"
./cryextsck "$IMG"

./cryexts_journal_inject "$IMG"
echo "pre-replay recovery image injected"

if ./cryextsck "$IMG"; then
	echo "expected pre-replay cryextsck to fail on needs_recovery image" >&2
	exit 1
fi
echo "pre-replay fsck failed as expected"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
echo "mount-time replay succeeded"
test -d "$MNT"
test "$(find "$MNT" -mindepth 1 -maxdepth 1 | wc -l)" -eq 0
sudo mkdir -p "$MNT/j1"
printf 'v4.2-journal-test\n' | sudo tee "$MNT/j1/a.txt" >/dev/null
test "$(cat "$MNT/j1/a.txt")" = "v4.2-journal-test"
sudo mv "$MNT/j1/a.txt" "$MNT/j1/a_renamed.txt"
test "$(cat "$MNT/j1/a_renamed.txt")" = "v4.2-journal-test"
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"
echo "post-replay fsck is clean"

trap - EXIT
echo "v4.2 journal smoke test passed"
