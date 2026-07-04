#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v5_5.img}
BAD_IMG=${BAD_IMG:-cryexts-v5_5-bad.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
FILE_COUNT=${FILE_COUNT:-180}
SRC=${SRC:-/tmp/cryexts-v5_5-src.bin}
TEST_MB=${TEST_MB:-24}
CHUNK_MB=${CHUNK_MB:-4}
FRAG_COUNT=${FRAG_COUNT:-8}
DIR=${DIR:-$MNT/bigdir}
TARGET=${TARGET:-$MNT/ext/overflow.bin}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$SRC" "$BAD_IMG" /tmp/cryexts-piece.bin
	rm -f /tmp/cryexts-frag-*.bin
	rm -f /tmp/cryexts-v5_5-policy.txt
	rm -f /tmp/cryexts-v5_5-dir-index.txt
	rm -f /tmp/cryexts-v5_5-extent.txt
}

trap cleanup EXIT

make
rm -f "$IMG" "$BAD_IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -P 7 -L v55meta "$IMG"
./cryextsck "$IMG"
./cryexts_policy_inspect "$IMG" | tee /tmp/cryexts-v5_5-policy.txt
grep -Eq '^policy_table_block=[1-9][0-9]*$' /tmp/cryexts-v5_5-policy.txt
grep -Eq '^default_policy=7$' /tmp/cryexts-v5_5-policy.txt

dd if=/dev/urandom of="$SRC" bs=1M count="$TEST_MB"

if lsmod | grep -q '^cryexts '; then
	sudo rmmod cryexts
fi
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$DIR" "$MNT/ext"

for i in $(seq 1 "$FILE_COUNT"); do
	sudo touch "$DIR/file_$i"
done

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
test -f "$DIR/file_1"
test -f "$DIR/file_$FILE_COUNT"
dir_ino=$(stat -c %i "$DIR")
file_ino=$(stat -c %i "$TARGET")

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_dir_index_inspect "$IMG" "$dir_ino" | tee /tmp/cryexts-v5_5-dir-index.txt
grep -Eq '^index_block=[1-9][0-9]*$' /tmp/cryexts-v5_5-dir-index.txt
./cryexts_extent_inspect "$IMG" "$file_ino" | tee /tmp/cryexts-v5_5-extent.txt
grep -Eq '^overflow_block=[1-9][0-9]*$' /tmp/cryexts-v5_5-extent.txt
grep -Eq '^overflow_entries=[1-9][0-9]*$' /tmp/cryexts-v5_5-extent.txt
./cryextsck "$IMG"

cp "$IMG" "$BAD_IMG"
index_block=$(awk -F= '/^index_block=/{print $2}' /tmp/cryexts-v5_5-dir-index.txt)
python3 - "$BAD_IMG" "$index_block" <<'PY'
import pathlib
import sys

img = pathlib.Path(sys.argv[1])
index_block = int(sys.argv[2])
offset = index_block * 4096 + 12

with img.open("r+b") as fp:
    fp.seek(offset)
    raw = fp.read(4)
    if len(raw) != 4:
        raise SystemExit("failed to read directory index checksum")
    fp.seek(offset)
    fp.write(bytes([raw[0] ^ 0x5A]) + raw[1:])
PY

if ./cryextsck "$BAD_IMG"; then
	echo "expected corrupted metadata image to fail fsck" >&2
	exit 1
fi

trap - EXIT
echo "v5.5 metadata-checksum smoke test passed"
