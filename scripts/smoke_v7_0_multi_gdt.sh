#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v7_0-multi-gdt.img}
BLOCK_SIZE=4096
BLOCKS_PER_GROUP=4096
GROUP_DESC_BYTES=76
SINGLE_GDT_GROUPS=$((BLOCK_SIZE / GROUP_DESC_BYTES))
TARGET_GROUPS=${TARGET_GROUPS:-$((SINGLE_GDT_GROUPS + 2))}
SIZE_MB=${SIZE_MB:-$((((TARGET_GROUPS * BLOCKS_PER_GROUP * BLOCK_SIZE) + 1024 * 1024 - 1) / 1024 / 1024))}
FSCK_LOG=${FSCK_LOG:-/tmp/cryexts-v7_0-fsck.log}

cleanup() {
	rm -f "$FSCK_LOG"
}

trap cleanup EXIT

make
rm -f "$IMG"

dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -M "$IMG"

INSPECT_OUTPUT=$(./cryexts_gdt_inspect "$IMG")
printf '%s\n' "$INSPECT_OUTPUT"

gdt_blocks=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="gdt_blocks"{print $2}')
expected_gdt_blocks=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="expected_gdt_blocks"{print $2}')
root_block_bitmap=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="root_block_bitmap"{print $2}')
group_count=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="group_count"{print $2}')
last_group_start=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="group['"$((TARGET_GROUPS - 1))"'].start"{print $2}')

test -n "$gdt_blocks"
test -n "$expected_gdt_blocks"
test -n "$root_block_bitmap"
test -n "$group_count"
test -n "$last_group_start"
test "$gdt_blocks" -gt 1
test "$gdt_blocks" = "$expected_gdt_blocks"
test "$group_count" -ge "$TARGET_GROUPS"
test "$root_block_bitmap" -gt 2

if ./cryextsck "$IMG" >"$FSCK_LOG" 2>&1; then
	echo "cryextsck unexpectedly accepted a multi-block GDT image" >&2
	exit 1
fi

grep -q "multi-block GDT is not yet supported by cryextsck" "$FSCK_LOG"

echo "v7.0 multi-GDT smoke test passed"
