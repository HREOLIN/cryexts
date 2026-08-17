#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v11_2.img}
CRASH_IMG=${CRASH_IMG:-cryexts-v11_2-crash.img}
MNT=${MNT:-/tmp/cryexts-v11_2-mnt}
SIZE_MB=${SIZE_MB:-128}
INSPECT=${INSPECT:-/tmp/cryexts-v11_2-journal.txt}
FSCK_LOG=${FSCK_LOG:-/tmp/cryexts-v11_2-fsck.txt}
MARKER=CRYEXTS_V11_V3_AFTER_IMAGE

log_step() {
	echo "[v11.2] $1"
}

inspect_value() {
	awk -F= -v key="$1" '$1 == key { print substr($0, length($1) + 2) }' \
		"$INSPECT"
}

assert_equal() {
	local left right

	left=$(inspect_value "$1")
	right=$(inspect_value "$2")
	if [[ -z "$left" || "$left" != "$right" ]]; then
		echo "journal inspect mismatch: $1=$left $2=$right" >&2
		exit 1
	fi
}

make_image() {
	local image=$1

	rm -f "$image"
	dd if=/dev/zero of="$image" bs=1M count="$SIZE_MB" status=none
	./mkfs.cryexts -f -G -X -A -I -T -M -R -P 7 -L v112redo "$image"
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

log_step "create clean runtime image"
make_image "$IMG"
./cryextsck "$IMG"

log_step "exercise journal v3 read-write transaction"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir "$MNT/redo"
sudo touch "$MNT/redo/after-image.txt"
sync
sudo umount "$MNT"
sudo rmmod cryexts

log_step "validate checkpointed runtime transaction"
./cryextsck "$IMG"
./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^journal_format=v3$' "$INSPECT"
grep -q '^control.state=0$' "$INSPECT"
grep -q '^control.idle=1$' "$INSPECT"
grep -q '^descriptor.entry_count=0$' "$INSPECT"
grep -q '^commit.entry_count=0$' "$INSPECT"
assert_equal control.checksum control.expected_checksum
assert_equal descriptor.checksum descriptor.expected_checksum
assert_equal descriptor.checksum commit.descriptor_checksum
assert_equal commit.checksum commit.expected_checksum

log_step "inject committed-before-checkpoint image"
make_image "$CRASH_IMG"
./cryextsck "$CRASH_IMG"
./cryexts_journal_v3_inject "$CRASH_IMG"
./cryexts_journal_inspect "$CRASH_IMG" | tee "$INSPECT"
grep -q '^control.state=2$' "$INSPECT"
grep -q '^descriptor.entry_count=1$' "$INSPECT"
grep -q '^commit.flags=1$' "$INSPECT"
grep -q '^commit.entry_count=1$' "$INSPECT"
assert_equal descriptor.checksum descriptor.expected_checksum
assert_equal descriptor.checksum commit.descriptor_checksum
assert_equal 'descriptor.entry[0].payload_checksum' \
	'descriptor.entry[0].expected_payload_checksum'
assert_equal commit.payload_checksum commit.expected_payload_checksum
assert_equal commit.checksum commit.expected_checksum

log_step "verify fsck recognizes replay-pending transaction"
if ./cryextsck "$CRASH_IMG" 2>&1 | tee "$FSCK_LOG"; then
	echo "replay-pending image unexpectedly passed fsck" >&2
	exit 1
fi
grep -q 'journal v3 committed transaction replay pending' "$FSCK_LOG"

log_step "verify after-image exists only in journal payload"
HOME_BLOCK=$(inspect_value 'descriptor.entry[0].home')
PAYLOAD_BLOCK=$(inspect_value 'descriptor.payload_start')
if dd if="$CRASH_IMG" bs=4096 skip="$HOME_BLOCK" count=1 status=none |
	grep -aFq "$MARKER"; then
	echo "home block was checkpointed unexpectedly" >&2
	exit 1
fi
dd if="$CRASH_IMG" bs=4096 skip="$PAYLOAD_BLOCK" count=1 status=none |
	grep -aFq "$MARKER"

trap - EXIT
cleanup
echo "v11.2 single-transaction redo commit smoke test passed"
