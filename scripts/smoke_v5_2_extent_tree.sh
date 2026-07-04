#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v5_2.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
SRC=${SRC:-/tmp/cryexts-v5_2-src.bin}
DST=${DST:-/tmp/cryexts-v5_2-dst.bin}
TEST_MB=${TEST_MB:-24}
TRUNCATE_SIZE=${TRUNCATE_SIZE:-7340032}
CHUNK_MB=${CHUNK_MB:-4}
FRAG_COUNT=${FRAG_COUNT:-8}
TARGET=${TARGET:-$MNT/ext/overflow.bin}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$SRC" "$DST" /tmp/cryexts-piece.bin /tmp/cryexts-v5_2-inspect.txt
	rm -f /tmp/cryexts-frag-*.bin
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -O -L v5ext "$IMG"
./cryextsck "$IMG"

dd if=/dev/urandom of="$SRC" bs=1M count="$TEST_MB"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$MNT/ext"

for i in $(seq 1 "$FRAG_COUNT"); do
	dd if=/dev/urandom of="/tmp/cryexts-frag-$i.bin" bs=1M count="$CHUNK_MB" status=none
	sudo cp "/tmp/cryexts-frag-$i.bin" "$MNT/ext/frag_$i.bin"
done
for i in $(seq 1 2 "$FRAG_COUNT"); do
	sudo rm -f "$MNT/ext/frag_$i.bin"
done

sudo rm -f "$TARGET"
offset=0
while [ "$offset" -lt "$TEST_MB" ]; do
	chunk="$CHUNK_MB"
	if [ $((offset + chunk)) -gt "$TEST_MB" ]; then
		chunk=$((TEST_MB - offset))
	fi
	dd if="$SRC" of="/tmp/cryexts-piece.bin" bs=1M skip="$offset" count="$chunk" status=none
	sudo dd if="/tmp/cryexts-piece.bin" of="$TARGET" bs=1M seek="$offset" conv=notrunc status=none
	offset=$((offset + chunk))
done

cmp "$SRC" "$TARGET"
inode_no=$(stat -c %i "$TARGET")

sudo sync
sudo umount "$MNT"
./cryexts_extent_inspect "$IMG" "$inode_no" | tee /tmp/cryexts-v5_2-inspect.txt
grep -Eq '^overflow_block=[1-9][0-9]*$' /tmp/cryexts-v5_2-inspect.txt
grep -Eq '^overflow_entries=[1-9][0-9]*$' /tmp/cryexts-v5_2-inspect.txt
sudo rmmod cryexts

./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo truncate -s "$TRUNCATE_SIZE" "$TARGET"
sudo cp "$TARGET" "$DST"
test "$(stat -c %s "$DST")" -eq "$TRUNCATE_SIZE"
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

trap - EXIT
echo "v5.2 extent overflow smoke test passed"
