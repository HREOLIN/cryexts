#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v11_1.img}
MNT=${MNT:-/tmp/cryexts-v11_1-mnt}
SIZE_MB=${SIZE_MB:-128}
INSPECT=${INSPECT:-/tmp/cryexts-v11_1-journal.txt}

log_step() {
	echo "[v11.1] $1"
}

inspect_value() {
	sed -n "s/^$1=//p" "$INSPECT"
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

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT" || true
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts || true
	fi
	rm -f "$INSPECT"
}

trap cleanup EXIT

log_step "build cryexts"
make

log_step "create clean journal v3 image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
./mkfs.cryexts -f -G -X -A -I -T -M -R -P 7 -L v111layout "$IMG"

log_step "validate clean image"
./cryextsck "$IMG"
./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^journal_format=v3$' "$INSPECT"
grep -q '^control.state=0$' "$INSPECT"
grep -q '^control.idle=1$' "$INSPECT"
grep -q '^control.checkpoint_complete=1$' "$INSPECT"
grep -q '^descriptor.entry_count=0$' "$INSPECT"
grep -q '^commit.entry_count=0$' "$INSPECT"
assert_equal control.checksum control.expected_checksum
assert_equal descriptor.checksum descriptor.expected_checksum
assert_equal descriptor.checksum commit.descriptor_checksum
assert_equal commit.checksum commit.expected_checksum
grep -q '^commit.payload_checksum=0$' "$INSPECT"

log_step "insert module"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"

log_step "verify clean journal v3 read-only mount"
sudo mount -o loop,ro -t cryexts "$IMG" "$MNT"
sudo ls -la "$MNT" >/dev/null
sudo umount "$MNT"
sudo rmmod cryexts

log_step "final fsck"
./cryextsck "$IMG"

trap - EXIT
rm -f "$INSPECT"
echo "v11.1 journal v3 layout smoke test passed"
