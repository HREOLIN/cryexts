#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v6_2.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
SRC=${SRC:-/tmp/cryexts-v6_2-src.bin}
DST=${DST:-/tmp/cryexts-v6_2-dst.bin}
INSPECT_OUT=${INSPECT_OUT:-/tmp/cryexts-v6_2-inspect.txt}
FRAG_BLOCK=${FRAG_BLOCK:-/tmp/cryexts-v6_2-frag-block.bin}
TEST_BLOCKS=${TEST_BLOCKS:-220}
TRUNCATE_SIZE=${TRUNCATE_SIZE:-524288}
TARGET_DIR=${TARGET_DIR:-$MNT/ext}
TARGET=${TARGET:-$TARGET_DIR/tree_v2.bin}
BLOCKER_PREFIX=${BLOCKER_PREFIX:-$TARGET_DIR/blocker}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$SRC" "$DST" "$INSPECT_OUT" "$FRAG_BLOCK"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -J -P 7 -L v62ext "$IMG"
./cryextsck "$IMG"

dd if=/dev/urandom of="$SRC" bs=4096 count="$TEST_BLOCKS"
dd if=/dev/urandom of="$FRAG_BLOCK" bs=4096 count=1

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$TARGET_DIR"

sudo rm -f "$TARGET"
block_index=0
while [ "$block_index" -lt "$TEST_BLOCKS" ]; do
	dd if="$SRC" of=/tmp/cryexts-v6_2-piece.bin bs=4096 skip="$block_index" count=1 status=none
	sudo dd if=/tmp/cryexts-v6_2-piece.bin of="$TARGET" bs=4096 seek="$block_index" count=1 conv=notrunc status=none
	sudo cp "$FRAG_BLOCK" "${BLOCKER_PREFIX}_$block_index.bin"
	block_index=$((block_index + 1))
done

cmp "$SRC" "$TARGET"
inode_no=$(stat -c %i "$TARGET")

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_extent_inspect "$IMG" "$inode_no" | tee "$INSPECT_OUT"
grep -q '^tree_v2=1$' "$INSPECT_OUT"
grep -Eq '^leaf_count=[2-9][0-9]*$' "$INSPECT_OUT"
grep -Eq '^root_ref\[1\]\.leaf_block=[1-9][0-9]*$' "$INSPECT_OUT"
grep -Eq '^leaf\[1\]\.header\.entries=[1-9][0-9]*$' "$INSPECT_OUT"

./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo truncate -s "$TRUNCATE_SIZE" "$TARGET"
sudo cp "$TARGET" "$DST"
test "$(stat -c %s "$DST")" -eq "$TRUNCATE_SIZE"
cmp <(head -c "$TRUNCATE_SIZE" "$SRC") "$DST"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

trap - EXIT
echo "v6.2 multi-leaf extent tree smoke test passed"
