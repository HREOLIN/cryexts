#!/usr/bin/env bash
set -euo pipefail

COMMITTED_IMG=${COMMITTED_IMG:-cryexts-v12_2-committed.img}
UNCOMMITTED_IMG=${UNCOMMITTED_IMG:-cryexts-v12_2-uncommitted.img}
MNT=${MNT:-/tmp/cryexts-v12_2-recovery-mnt}
SIZE_MB=${SIZE_MB:-128}
INSPECT=${INSPECT:-/tmp/cryexts-v12_2-recovery-journal.txt}
FSCK_LOG=${FSCK_LOG:-/tmp/cryexts-v12_2-recovery-fsck.txt}
MARKER1=CRYEXTS_RING_TXN_1
MARKER2=CRYEXTS_RING_TXN_2

log_step() {
	echo "[v12.2] $1"
}

inspect_value() {
	awk -F= -v key="$1" '$1 == key { print substr($0, length($1) + 2) }' \
		"$INSPECT"
}

make_ring_image() {
	rm -f "$1"
	dd if=/dev/zero of="$1" bs=1M count="$SIZE_MB" status=none
	./mkfs.cryexts -f -G -X -A -I -T -M -Q -P 7 -L v122recovery "$1"
}

inspect_image() {
	./cryexts_journal_inspect "$1" | tee "$INSPECT"
}

block_has_marker() {
	dd if="$1" bs=4096 skip="$2" count=1 status=none | grep -aFq "$3"
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
	grep -q '^control.checkpoint_complete=1$' "$INSPECT"
	HEAD=$(inspect_value 'control.ring_head')
	TAIL=$(inspect_value 'control.ring_tail')
	test -n "$HEAD" && test "$HEAD" = "$TAIL"
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

log_step "replay multiple committed ring transactions"
make_ring_image "$COMMITTED_IMG"
./cryexts_journal_v3_ring_inject "$COMMITTED_IMG" committed
inspect_image "$COMMITTED_IMG"
grep -q '^control.state=3$' "$INSPECT"
grep -q '^commit.flags=1$' "$INSPECT"
HEAD=$(inspect_value 'control.ring_head')
TAIL=$(inspect_value 'control.ring_tail')
test -n "$HEAD" && test -n "$TAIL" && test "$HEAD" != "$TAIL"
HOME_BLOCK=$(inspect_value 'descriptor.entry[0].home')
! block_has_marker "$COMMITTED_IMG" "$HOME_BLOCK" "$MARKER2"
if ./cryextsck "$COMMITTED_IMG" >"$FSCK_LOG" 2>&1; then
	echo "committed ring image unexpectedly passed fsck before replay" >&2
	exit 1
fi
mount_once "$COMMITTED_IMG"
block_has_marker "$COMMITTED_IMG" "$HOME_BLOCK" "$MARKER2"
! block_has_marker "$COMMITTED_IMG" "$HOME_BLOCK" "$MARKER1"
assert_clean "$COMMITTED_IMG"

log_step "discard uncommitted tail ring transaction"
make_ring_image "$UNCOMMITTED_IMG"
./cryexts_journal_v3_ring_inject "$UNCOMMITTED_IMG" uncommitted-tail
inspect_image "$UNCOMMITTED_IMG"
grep -q '^control.state=2$' "$INSPECT"
grep -q '^commit.flags=0$' "$INSPECT"
HEAD=$(inspect_value 'control.ring_head')
TAIL=$(inspect_value 'control.ring_tail')
test -n "$HEAD" && test -n "$TAIL" && test "$HEAD" != "$TAIL"
HOME_BLOCK=$(inspect_value 'descriptor.entry[0].home')
! block_has_marker "$UNCOMMITTED_IMG" "$HOME_BLOCK" "$MARKER1"
if ./cryextsck "$UNCOMMITTED_IMG" >"$FSCK_LOG" 2>&1; then
	echo "uncommitted ring image unexpectedly passed fsck before discard" >&2
	exit 1
fi
mount_once "$UNCOMMITTED_IMG"
block_has_marker "$UNCOMMITTED_IMG" "$HOME_BLOCK" "$MARKER1"
! block_has_marker "$UNCOMMITTED_IMG" "$HOME_BLOCK" "$MARKER2"
assert_clean "$UNCOMMITTED_IMG"

trap - EXIT
cleanup
echo "v12.2 ring recovery smoke test passed"
