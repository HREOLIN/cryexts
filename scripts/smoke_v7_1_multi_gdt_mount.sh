#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v7_1-multi-gdt.img}
MNT=${MNT:-/tmp/cryexts-mnt}
BLOCK_SIZE=4096
GROUP_DESC_BYTES=76
BLOCKS_PER_GROUP=4096
DESCS_PER_BLOCK=$((BLOCK_SIZE / GROUP_DESC_BYTES))
TARGET_GROUPS=${TARGET_GROUPS:-$((DESCS_PER_BLOCK + 2))}
SIZE_MB=${SIZE_MB:-$((((TARGET_GROUPS * BLOCKS_PER_GROUP * BLOCK_SIZE) + 1024 * 1024 - 1) / 1024 / 1024))}
TARGET_GROUP_INDEX=${TARGET_GROUP_INDEX:-$DESCS_PER_BLOCK}
FANOUT=${FANOUT:-64}
INODE_MARGIN=${INODE_MARGIN:-128}
INSPECT_BEFORE=${INSPECT_BEFORE:-/tmp/cryexts-v7_1-before.txt}
INSPECT_AFTER=${INSPECT_AFTER:-/tmp/cryexts-v7_1-after.txt}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$INSPECT_BEFORE" "$INSPECT_AFTER"
}

trap cleanup EXIT

make
rm -f "$IMG"

dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -M "$IMG"
./cryexts_gdt_inspect "$IMG" | tee "$INSPECT_BEFORE"

gdt_blocks_before=$(awk -F= '$1=="gdt_blocks"{print $2}' "$INSPECT_BEFORE")
expected_gdt_blocks=$(awk -F= '$1=="expected_gdt_blocks"{print $2}' "$INSPECT_BEFORE")
free_inodes_before=$(awk -F= '$1=="group['"$TARGET_GROUP_INDEX"'].free_inodes"{print $2}' "$INSPECT_BEFORE")
required_inode_allocations=$(awk -F= -v target="$TARGET_GROUP_INDEX" '
	$1 ~ /^group\[[0-9]+\]\.free_inodes$/ {
		split($1, parts, /[\[\]\.]/);
		group = parts[2] + 0;
		if (group < target)
			sum += $2 + 0;
	}
	END {
		if (sum == "")
			sum = 0;
		print sum + 1;
	}
' "$INSPECT_BEFORE")
total_free_inodes=$(awk -F= '
	$1 ~ /^group\[[0-9]+\]\.free_inodes$/ {
		sum += $2 + 0;
	}
	END {
		print sum;
	}
' "$INSPECT_BEFORE")
target_total_allocations=$((required_inode_allocations + INODE_MARGIN))
if [ "$target_total_allocations" -gt "$total_free_inodes" ]; then
	target_total_allocations=$total_free_inodes
fi
file_count=$target_total_allocations
while :; do
	dir_count=$(((file_count + FANOUT - 1) / FANOUT))
	total_inode_allocations=$((file_count + dir_count))
	if [ "$total_inode_allocations" -gt "$target_total_allocations" ]; then
		file_count=$((file_count - 1))
		continue
	fi
	if [ "$total_inode_allocations" -lt "$required_inode_allocations" ]; then
		echo "unable to reach target group $TARGET_GROUP_INDEX with current FANOUT=$FANOUT" >&2
		exit 1
	fi
	break
done
last_index=$((file_count - 1))
last_dir_index=$((last_index / FANOUT))
last_dir=$(printf "%03d" "$last_dir_index")
last_file=$(printf "%05d" "$last_index")

test -n "$gdt_blocks_before"
test -n "$expected_gdt_blocks"
test -n "$free_inodes_before"
test -n "$required_inode_allocations"
test -n "$total_free_inodes"
test "$gdt_blocks_before" -gt 1
test "$gdt_blocks_before" = "$expected_gdt_blocks"
test "$TARGET_GROUP_INDEX" -ge "$DESCS_PER_BLOCK"
test "$total_inode_allocations" -ge "$required_inode_allocations"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo python3 - "$MNT" "$file_count" "$FANOUT" <<'PY'
import os
import sys

mnt = sys.argv[1]
count = int(sys.argv[2])
fanout = int(sys.argv[3])

dir_count = (count + fanout - 1) // fanout

for d in range(dir_count):
    os.mkdir(os.path.join(mnt, f"shard_{d:03d}"))

for i in range(count):
    d = i // fanout
    path = os.path.join(mnt, f"shard_{d:03d}", f"inode_{i:05d}")
    with open(path, "wb"):
        pass

probe = os.path.join(mnt, f"shard_{(count - 1) // fanout:03d}",
                     f"inode_{count - 1:05d}")
if not os.path.exists(probe):
    raise SystemExit(f"missing probe file: {probe}")
PY

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_gdt_inspect "$IMG" | tee "$INSPECT_AFTER"

gdt_blocks_after=$(awk -F= '$1=="gdt_blocks"{print $2}' "$INSPECT_AFTER")
free_inodes_after=$(awk -F= '$1=="group['"$TARGET_GROUP_INDEX"'].free_inodes"{print $2}' "$INSPECT_AFTER")
checksum_after=$(awk -F= '$1=="group['"$TARGET_GROUP_INDEX"'].checksum"{print $2}' "$INSPECT_AFTER")
expected_checksum_after=$(awk -F= '$1=="group['"$TARGET_GROUP_INDEX"'].expected_checksum"{print $2}' "$INSPECT_AFTER")

test -n "$gdt_blocks_after"
test -n "$free_inodes_after"
test -n "$checksum_after"
test -n "$expected_checksum_after"
test "$gdt_blocks_after" = "$gdt_blocks_before"
test "$free_inodes_after" -lt "$free_inodes_before"
test "$checksum_after" = "$expected_checksum_after"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
test -f "$MNT/shard_000/inode_00000"
test -f "$MNT/shard_$last_dir/inode_$last_file"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

trap - EXIT
echo "v7.1 multi-GDT mount smoke test passed"
