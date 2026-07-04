#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v5_6.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
SRC=${SRC:-/tmp/cryexts-v5_6-src.bin}
TARGET=${TARGET:-$MNT/locality/seq.bin}
FRAG_DIR=${FRAG_DIR:-$MNT/locality/frags}
TEST_MB=${TEST_MB:-16}
CHUNK_MB=${CHUNK_MB:-2}
FRAG_COUNT=${FRAG_COUNT:-6}
SIB_COUNT=${SIB_COUNT:-6}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$SRC" /tmp/cryexts-v5_6-piece.bin /tmp/cryexts-v5_6-extent.txt
	rm -f /tmp/cryexts-v5_6-siblings.txt
	rm -f /tmp/cryexts-v5_6-frag-*.bin
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -I -T -M -P 7 -L v56local "$IMG"
./cryextsck "$IMG"

dd if=/dev/urandom of="$SRC" bs=1M count="$TEST_MB"

if lsmod | grep -q '^cryexts '; then
	sudo rmmod cryexts
fi
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$MNT/locality" "$FRAG_DIR"

for i in $(seq 1 "$FRAG_COUNT"); do
	dd if=/dev/urandom of="/tmp/cryexts-v5_6-frag-$i.bin" bs=1M count="$CHUNK_MB" status=none
	sudo cp "/tmp/cryexts-v5_6-frag-$i.bin" "$FRAG_DIR/frag_$i.bin"
done
for i in $(seq 1 2 "$FRAG_COUNT"); do
	sudo rm -f "$FRAG_DIR/frag_$i.bin"
done

sudo rm -f "$TARGET"
offset=0
while [ "$offset" -lt "$TEST_MB" ]; do
	chunk="$CHUNK_MB"
	if [ $((offset + chunk)) -gt "$TEST_MB" ]; then
		chunk=$((TEST_MB - offset))
	fi
	dd if="$SRC" of="/tmp/cryexts-v5_6-piece.bin" bs=1M skip="$offset" count="$chunk" status=none
	sudo dd if="/tmp/cryexts-v5_6-piece.bin" of="$TARGET" bs=1M seek="$offset" conv=notrunc status=none
	offset=$((offset + chunk))
done

cmp "$SRC" "$TARGET"
target_ino=$(stat -c %i "$TARGET")

for i in $(seq 1 "$SIB_COUNT"); do
	printf 'sibling-%s\n' "$i" | sudo tee "$MNT/locality/sibling_$i" >/dev/null
done

python3 - <<'PY' | tee /tmp/cryexts-v5_6-siblings.txt
import os
from pathlib import Path

base = Path("/tmp/cryexts-mnt/locality")
for child in sorted(base.iterdir()):
    st = child.stat()
    print(f"{child.name} {st.st_ino}")
PY

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_extent_inspect "$IMG" "$target_ino" | tee /tmp/cryexts-v5_6-extent.txt
grep -Eq '^inline_entries=[1-9][0-9]*$' /tmp/cryexts-v5_6-extent.txt

python3 - <<'PY'
from pathlib import Path
import re

lines = Path("/tmp/cryexts-v5_6-extent.txt").read_text().splitlines()
segments = []
for line in lines:
    m = re.match(r'^(inline|overflow)\[\d+\]: logical=(\d+) physical=(\d+) len=(\d+) flags=(\d+)$', line)
    if m:
        segments.append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4))))

if not segments:
    raise SystemExit("no extent entries found")

max_len = max(seg[3] for seg in segments)
if max_len < 128:
    raise SystemExit(f"largest contiguous extent too small: {max_len}")

print(f"largest_extent_len={max_len}")
print(f"extent_segments={len(segments)}")
PY

python3 - <<'PY'
from pathlib import Path
import struct

IMG = Path("cryexts-v5_6.img")
siblings = []
for line in Path("/tmp/cryexts-v5_6-siblings.txt").read_text().splitlines():
    name, ino = line.split()
    if name.startswith("sibling_"):
        siblings.append((name, int(ino)))

data = IMG.read_bytes()
sb = data[1024:1024 + 4096 - 1024]

def le16(off):
    return struct.unpack_from("<H", sb, off)[0]

def le32(off):
    return struct.unpack_from("<I", sb, off)[0]

def le64(off):
    return struct.unpack_from("<Q", sb, off)[0]

features_incompat = le32(88)
blocks_per_group = le64(184)
inodes_per_group = le64(192)
inode_table_blocks = le64(80)
inode_size = le32(12)
group_desc_table_start = le64(200)
use_groups = bool(features_incompat & 0x00000002)
extent_flag = 0x00000001
group_desc_size = 76

gdt = data[group_desc_table_start * 4096:(group_desc_table_start + 1) * 4096]

def inode_offset(ino):
    index = ino - 1
    if use_groups:
        group = index // inodes_per_group
        index_in_group = index % inodes_per_group
        desc_off = group * group_desc_size
        inode_table_start = struct.unpack_from("<Q", gdt, desc_off + 32)[0]
        ipb = 4096 // inode_size
        block = inode_table_start + index_in_group // ipb
        off = (index_in_group % ipb) * inode_size
        return block * 4096 + off
    inode_table_start = le64(64)
    ipb = 4096 // inode_size
    block = inode_table_start + index // ipb
    off = (index % ipb) * inode_size
    return block * 4096 + off

groups = set()
for name, ino in siblings:
    off = inode_offset(ino)
    inode = data[off:off + inode_size]
    inode_flags = struct.unpack_from("<I", inode, 156)[0]
    first_block = 0
    if inode_flags & extent_flag:
        first_block = struct.unpack_from("<Q", inode, 176)[0]
    else:
        first_block = struct.unpack_from("<Q", inode, 52)[0]
    if not first_block:
        raise SystemExit(f"{name} has no allocated first block")
    groups.add(first_block // blocks_per_group)

if len(groups) > 2:
    raise SystemExit(f"sibling files spread across too many groups: {sorted(groups)}")

print(f"sibling_groups={sorted(groups)}")
PY

./cryextsck "$IMG"

trap - EXIT
echo "v5.6 prealloc-locality smoke test passed"
