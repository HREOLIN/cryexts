#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v11_0.img}
MNT=${MNT:-/tmp/cryexts-v11_0-mnt}
SIZE_MB=${SIZE_MB:-128}
INSPECT=/tmp/cryexts-v11_0-journal.txt
MARKER=CRYEXTS_V11_V2_COMMITTED_HOME

log_step() {
	echo "[v11.0] $1"
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

log_step "create clean journal v2 image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
./mkfs.cryexts -f -G -X -A -I -T -M -J -P 7 -L v110crash "$IMG"
./cryextsck "$IMG"

if grep -aFq "$MARKER" "$IMG"; then
	echo "rollback marker unexpectedly exists before injection" >&2
	exit 1
fi

log_step "inject committed-home rollback window"
./cryexts_journal_v2_inject "$IMG" rollback-window
grep -aFq "$MARKER" "$IMG"

./cryexts_journal_inspect "$IMG" | tee "$INSPECT"
grep -q '^control.active_sequence=[1-9][0-9]*$' "$INSPECT"
grep -q '^commit.flags=2$' "$INSPECT"
grep -q '^control.checkpoint_complete=0$' "$INSPECT"

log_step "verify fsck reports replay pending"
if ./cryextsck "$IMG"; then
	echo "expected cryextsck to reject replay-pending image" >&2
	exit 1
fi

log_step "mount and reproduce v2 rollback"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo umount "$MNT"
sudo rmmod cryexts

if grep -aFq "$MARKER" "$IMG"; then
	echo "v2 replay did not restore the old payload" >&2
	exit 1
fi

log_step "verify replayed image"
./cryextsck "$IMG"

trap - EXIT
rm -f "$INSPECT"
echo "v11.0 journal v2 crash-window baseline smoke test passed"
