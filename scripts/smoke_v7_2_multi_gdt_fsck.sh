#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v7_2-multi-gdt.img}
BLOCK_SIZE=4096
BLOCKS_PER_GROUP=4096
GROUP_DESC_BYTES=76
DESCS_PER_BLOCK=$((BLOCK_SIZE / GROUP_DESC_BYTES))
TARGET_GROUPS=${TARGET_GROUPS:-$((DESCS_PER_BLOCK + 2))}
SIZE_MB=${SIZE_MB:-$((((TARGET_GROUPS * BLOCKS_PER_GROUP * BLOCK_SIZE) + 1024 * 1024 - 1) / 1024 / 1024))}
FSCK_LOG=${FSCK_LOG:-/tmp/cryexts-v7_2-fsck.log}

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
group_count=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="group_count"{print $2}')
last_group_start=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="group['"$((TARGET_GROUPS - 1))"'].start"{print $2}')
last_group_checksum=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="group['"$((TARGET_GROUPS - 1))"'].checksum"{print $2}')
last_group_expected_checksum=$(printf '%s\n' "$INSPECT_OUTPUT" | awk -F= '$1=="group['"$((TARGET_GROUPS - 1))"'].expected_checksum"{print $2}')

test -n "$gdt_blocks"
test -n "$expected_gdt_blocks"
test -n "$group_count"
test -n "$last_group_start"
test -n "$last_group_checksum"
test -n "$last_group_expected_checksum"
test "$gdt_blocks" -gt 1
test "$gdt_blocks" = "$expected_gdt_blocks"
test "$group_count" -ge "$TARGET_GROUPS"
test "$last_group_checksum" = "$last_group_expected_checksum"

./cryextsck "$IMG" | tee "$FSCK_LOG"
grep -q "cryextsck: $IMG clean" "$FSCK_LOG"

echo "v7.2 multi-GDT fsck smoke test passed"
