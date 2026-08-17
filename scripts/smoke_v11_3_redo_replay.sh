#!/usr/bin/env bash
set -euo pipefail

COMMITTED_IMG=${COMMITTED_IMG:-cryexts-v11_3-committed.img}
UNCOMMITTED_IMG=${UNCOMMITTED_IMG:-cryexts-v11_3-uncommitted.img}
PARTIAL_IMG=${PARTIAL_IMG:-cryexts-v11_3-partial.img}
MNT=${MNT:-/tmp/cryexts-v11_3-mnt}
SIZE_MB=${SIZE_MB:-128}
INSPECT=${INSPECT:-/tmp/cryexts-v11_3-journal.txt}
FSCK_LOG=${FSCK_LOG:-/tmp/cryexts-v11_3-fsck.txt}
MARKER=CRYEXTS_V11_V3_AFTER_IMAGE

log_step() {
	echo "[v11.3] $1"
}

inspect_value() {
	awk -F= -v key="$1" '$1 == key { print substr($0, length($1) + 2) }' \
		"$INSPECT"
}

make_image() {
	rm -f "$1"
	dd if=/dev/zero of="$1" bs=1M count="$SIZE_MB" status=none
	./mkfs.cryexts -f -G -X -A -I -T -M -R -P 7 -L v113redo "$1"
}

inspect_image() {
	./cryexts_journal_inspect "$1" | tee "$INSPECT"
}

block_has_marker() {
	dd if="$1" bs=4096 skip="$2" count=1 status=none | grep -aFq "$MARKER"
}

mount_once() {
	sudo mount -o loop -t cryexts "$1" "$MNT"
	sync
	sudo umount "$MNT"
}

assert_clean() {
	./cryextsck "$1"
	inspect_image "$1"
	grep -q '^control.state=0$' "$INSPECT"
	grep -q '^control.idle=1$' "$INSPECT"
	grep -q '^descriptor.entry_count=0$' "$INSPECT"
	grep -q '^commit.entry_count=0$' "$INSPECT"
}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT" || true
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts || true
	fi
	rm -f "$INSPECT" "$FSCK_LOG"
}

trap cleanup EXIT

log_step "build cryexts"
make
sudo mkdir -p "$MNT"
sudo insmod cryexts.ko

log_step "replay committed-before-checkpoint transaction"
make_image "$COMMITTED_IMG"
./cryexts_journal_v3_inject "$COMMITTED_IMG" committed
inspect_image "$COMMITTED_IMG"
grep -q '^control.state=2$' "$INSPECT"
grep -q '^commit.flags=1$' "$INSPECT"
HOME_BLOCK=$(inspect_value 'descriptor.entry[0].home')
PAYLOAD_BLOCK=$(inspect_value 'descriptor.payload_start')
! block_has_marker "$COMMITTED_IMG" "$HOME_BLOCK"
block_has_marker "$COMMITTED_IMG" "$PAYLOAD_BLOCK"
if ./cryextsck "$COMMITTED_IMG" 2>&1 | tee "$FSCK_LOG"; then
	echo "committed image unexpectedly passed fsck before replay" >&2
	exit 1
fi
grep -q 'journal v3 committed transaction replay pending' "$FSCK_LOG"
mount_once "$COMMITTED_IMG"
block_has_marker "$COMMITTED_IMG" "$HOME_BLOCK"
assert_clean "$COMMITTED_IMG"

log_step "discard uncommitted transaction"
make_image "$UNCOMMITTED_IMG"
./cryexts_journal_v3_inject "$UNCOMMITTED_IMG" uncommitted
inspect_image "$UNCOMMITTED_IMG"
grep -q '^control.state=2$' "$INSPECT"
grep -q '^commit.flags=0$' "$INSPECT"
HOME_BLOCK=$(inspect_value 'descriptor.entry[0].home')
! block_has_marker "$UNCOMMITTED_IMG" "$HOME_BLOCK"
if ./cryextsck "$UNCOMMITTED_IMG" 2>&1 | tee "$FSCK_LOG"; then
	echo "uncommitted image unexpectedly passed fsck before discard" >&2
	exit 1
fi
grep -q 'journal v3 uncommitted transaction pending' "$FSCK_LOG"
mount_once "$UNCOMMITTED_IMG"
! block_has_marker "$UNCOMMITTED_IMG" "$HOME_BLOCK"
assert_clean "$UNCOMMITTED_IMG"

log_step "replay partial checkpoint idempotently"
make_image "$PARTIAL_IMG"
./cryexts_journal_v3_inject "$PARTIAL_IMG" partial
inspect_image "$PARTIAL_IMG"
grep -q '^control.state=4$' "$INSPECT"
grep -q '^commit.flags=1$' "$INSPECT"
HOME_BLOCK=$(inspect_value 'descriptor.entry[0].home')
block_has_marker "$PARTIAL_IMG" "$HOME_BLOCK"
mount_once "$PARTIAL_IMG"
block_has_marker "$PARTIAL_IMG" "$HOME_BLOCK"
assert_clean "$PARTIAL_IMG"

trap - EXIT
cleanup
echo "v11.3 redo replay and idempotent recovery smoke test passed"
