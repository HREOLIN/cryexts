#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v4_3.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-96}
TEST_MB=${TEST_MB:-8}
SRC=${SRC:-/tmp/cryexts-v4_3-src.bin}
DST=${DST:-/tmp/cryexts-v4_3-dst.bin}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$SRC" "$DST"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -L v4ext "$IMG"
./cryextsck "$IMG"

dd if=/dev/urandom of="$SRC" bs=1M count="$TEST_MB"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$MNT/ext"
sudo cp "$SRC" "$MNT/ext/large.bin"
cmp "$SRC" "$MNT/ext/large.bin"
sudo sync
sudo truncate -s 3145728 "$MNT/ext/large.bin"
sudo cp "$MNT/ext/large.bin" "$DST"
test "$(stat -c %s "$DST")" -eq 3145728
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo cp "$MNT/ext/large.bin" "$DST"
test "$(stat -c %s "$DST")" -eq 3145728
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

trap - EXIT
echo "v4.3 extent smoke test passed"
