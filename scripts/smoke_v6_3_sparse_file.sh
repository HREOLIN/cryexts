#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v6_3.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
TARGET_DIR=${TARGET_DIR:-$MNT/sparse}
SPARSE_FILE=${SPARSE_FILE:-$TARGET_DIR/leading-hole.bin}
PUNCH_FILE=${PUNCH_FILE:-$TARGET_DIR/punch-hole.bin}
INSPECT_SPARSE=${INSPECT_SPARSE:-/tmp/cryexts-v6_3-sparse-inspect.txt}
INSPECT_PUNCH=${INSPECT_PUNCH:-/tmp/cryexts-v6_3-punch-inspect.txt}
READ_HOLE=${READ_HOLE:-/tmp/cryexts-v6_3-hole.bin}
READ_PUNCHED=${READ_PUNCHED:-/tmp/cryexts-v6_3-punched.bin}
ZERO_4K=${ZERO_4K:-/tmp/cryexts-v6_3-zero4k.bin}
TAIL_BLOCK=${TAIL_BLOCK:-/tmp/cryexts-v6_3-tail.bin}
PUNCH_SRC=${PUNCH_SRC:-/tmp/cryexts-v6_3-punch-src.bin}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$INSPECT_SPARSE" "$INSPECT_PUNCH" "$READ_HOLE" "$READ_PUNCHED"
	rm -f "$ZERO_4K" "$TAIL_BLOCK" "$PUNCH_SRC"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -J -P 7 -L v63sparse "$IMG"
./cryextsck "$IMG"

dd if=/dev/zero of="$ZERO_4K" bs=4096 count=1 status=none
dd if=/dev/zero of="$TAIL_BLOCK" bs=4096 count=1 status=none
printf 'cryexts-v6.3 sparse tail block\n' | dd of="$TAIL_BLOCK" conv=notrunc status=none
dd if=/dev/urandom of="$PUNCH_SRC" bs=4096 count=4 status=none

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$TARGET_DIR"

sudo dd if="$TAIL_BLOCK" of="$SPARSE_FILE" bs=4096 seek=256 count=1 conv=notrunc status=none
test "$(stat -c %s "$SPARSE_FILE")" -eq 1052672
dd if="$SPARSE_FILE" of="$READ_HOLE" bs=4096 skip=0 count=1 status=none
cmp "$ZERO_4K" "$READ_HOLE"
test "$(stat -c %b "$SPARSE_FILE")" -lt $((257 * 8))
sparse_inode=$(stat -c %i "$SPARSE_FILE")

sudo cp "$PUNCH_SRC" "$PUNCH_FILE"
sudo fallocate -p -o 4096 -l 4096 "$PUNCH_FILE"
test "$(stat -c %s "$PUNCH_FILE")" -eq 16384
dd if="$PUNCH_FILE" of="$READ_PUNCHED" bs=4096 skip=1 count=1 status=none
cmp "$ZERO_4K" "$READ_PUNCHED"
punch_inode=$(stat -c %i "$PUNCH_FILE")

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_extent_inspect "$IMG" "$sparse_inode" | tee "$INSPECT_SPARSE"
grep -q '^tree_v2=1$' "$INSPECT_SPARSE"
grep -Eq 'logical=256 ' "$INSPECT_SPARSE"
grep -Eq '^inode_blocks=([1-9]|[1-9][0-9])$' "$INSPECT_SPARSE"

./cryexts_extent_inspect "$IMG" "$punch_inode" | tee "$INSPECT_PUNCH"
grep -q '^tree_v2=1$' "$INSPECT_PUNCH"
grep -Eq 'logical=0 .* len=1 ' "$INSPECT_PUNCH"
grep -Eq 'logical=2 .* len=2 ' "$INSPECT_PUNCH"
if grep -Eq 'logical=1 ' "$INSPECT_PUNCH"; then
	echo "punched logical block is still mapped" >&2
	exit 1
fi

./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
dd if="$SPARSE_FILE" of="$READ_HOLE" bs=4096 skip=0 count=1 status=none
cmp "$ZERO_4K" "$READ_HOLE"
dd if="$PUNCH_FILE" of="$READ_PUNCHED" bs=4096 skip=1 count=1 status=none
cmp "$ZERO_4K" "$READ_PUNCHED"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

trap - EXIT
echo "v6.3 sparse file and punch-hole smoke test passed"
