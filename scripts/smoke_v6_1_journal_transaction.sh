#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v6_1.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
OUT1=/tmp/cryexts-v6_1-before.txt
OUT2=/tmp/cryexts-v6_1-after-write.txt
OUT3=/tmp/cryexts-v6_1-after-replay.txt

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$OUT1" "$OUT2" "$OUT3"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -J -P 7 -L v61txn "$IMG"
./cryexts_journal_inspect "$IMG" | tee "$OUT1"

grep -q '^control.active_sequence=0$' "$OUT1"
grep -q '^control.checkpoint_complete=1$' "$OUT1"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
printf 'v6.1-journal-txn\n' | sudo tee "$MNT/txn.txt" >/dev/null
test "$(cat "$MNT/txn.txt")" = "v6.1-journal-txn"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_journal_inspect "$IMG" | tee "$OUT2"
grep -q '^control.active_sequence=0$' "$OUT2"
grep -q '^control.checkpoint_complete=1$' "$OUT2"
./cryextsck "$IMG"

./cryexts_journal_v2_inject "$IMG"
echo "pre-replay v6.1 recovery image injected"

if ./cryextsck "$IMG"; then
	echo "expected pre-replay cryextsck to fail on v6.1 recovery image" >&2
	exit 1
fi
echo "pre-replay fsck failed as expected"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
echo "mount-time v6.1 replay succeeded"
test -d "$MNT"
test -f "$MNT/txn.txt"
test "$(cat "$MNT/txn.txt")" = "v6.1-journal-txn"
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_journal_inspect "$IMG" | tee "$OUT3"
grep -q '^control.active_sequence=0$' "$OUT3"
grep -q '^control.checkpoint_complete=1$' "$OUT3"
./cryextsck "$IMG"

trap - EXIT
echo "v6.1 journal-transaction smoke test passed"
