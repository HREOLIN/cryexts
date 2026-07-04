#!/usr/bin/env bash
set -euo pipefail

DEMO_MODE=${DEMO_MODE:-auto}
USB_HOST_DIR=${USB_HOST_DIR:-/media/$USER/USB}
IMG_NAME=${IMG_NAME:-cryexts-v7_0-demo.img}
IMG_PATH=${IMG_PATH:-$USB_HOST_DIR/$IMG_NAME}
TARGET_DEVICE=${TARGET_DEVICE:-}
ACK_RAW_DEVICE=${ACK_RAW_DEVICE:-}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-256}
LABEL=${LABEL:-v70demo}
KEY=${KEY:-}
LARGE_MB=${LARGE_MB:-8}
SPARSE_SIZE_MB=${SPARSE_SIZE_MB:-64}
DIR_FILE_COUNT=${DIR_FILE_COUNT:-96}
LARGE_SRC=${LARGE_SRC:-/tmp/cryexts-v7_0-large.bin}
LARGE_DST=${LARGE_DST:-/tmp/cryexts-v7_0-large.out}
BLOCK_SIZE=4096
BLOCKS_PER_GROUP=4096
GROUP_DESC_BYTES=76
MAX_GDT_GROUPS=$((BLOCK_SIZE / GROUP_DESC_BYTES))
MAX_RAW_DEVICE_BYTES=$((MAX_GDT_GROUPS * BLOCKS_PER_GROUP * BLOCK_SIZE))
CURRENT_STEP="startup"

log_step() {
	CURRENT_STEP=$1
	echo "[v7.0] $CURRENT_STEP"
}

on_error() {
	local line_no=$1
	echo "[v7.0] failed at step: $CURRENT_STEP (line $line_no)" >&2
	echo "[v7.0] hint: inspect kernel log with: dmesg | tail -n 120" >&2
}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$LARGE_SRC" "$LARGE_DST"
}

run_mkfs() {
	if [ "$DEMO_MODE" = "raw" ]; then
		sudo ./mkfs.cryexts "${MKFS_ARGS[@]}" "$TARGET_FS"
	else
		./mkfs.cryexts "${MKFS_ARGS[@]}" "$TARGET_FS"
	fi
}

run_fsck() {
	if [ "$DEMO_MODE" = "raw" ]; then
		sudo ./cryextsck "$TARGET_FS"
	else
		./cryextsck "$TARGET_FS"
	fi
}

mount_cryexts() {
	local target=$1

	if [ -n "$KEY" ]; then
		if [ "$DEMO_MODE" = "image" ]; then
			sudo mount -o loop,key="$KEY" -t cryexts "$target" "$MNT"
		else
			sudo mount -t cryexts -o key="$KEY" "$target" "$MNT"
		fi
	else
		if [ "$DEMO_MODE" = "image" ]; then
			sudo mount -o loop -t cryexts "$target" "$MNT"
		else
			sudo mount -t cryexts "$target" "$MNT"
		fi
	fi
}

require_raw_partition() {
	case "$TARGET_DEVICE" in
		/dev/sd[a-z][0-9]*|/dev/nvme[0-9]n[0-9]p[0-9]*|/dev/mmcblk[0-9]p[0-9]*|/dev/loop[0-9]*)
			;;
		*)
			echo "refusing unsafe raw target: $TARGET_DEVICE" >&2
			echo "use a partition-like device such as /dev/sdb1" >&2
			exit 2
			;;
	esac

	if [ "$ACK_RAW_DEVICE" != "I_UNDERSTAND_THE_RISK" ]; then
		echo "raw-device mode requires ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK" >&2
		exit 2
	fi
}

check_raw_device_size() {
	local size_bytes
	local size_mib
	local max_mib

	size_bytes=$(blockdev --getsize64 "$TARGET_DEVICE")
	size_mib=$((size_bytes / 1024 / 1024))
	max_mib=$((MAX_RAW_DEVICE_BYTES / 1024 / 1024))

	if [ "$size_bytes" -gt "$MAX_RAW_DEVICE_BYTES" ]; then
		echo "raw-device target is too large for current mkfs.cryexts GDT implementation" >&2
		echo "target: $TARGET_DEVICE (${size_mib} MiB)" >&2
		echo "current max raw-device size: ${max_mib} MiB" >&2
		echo "current mkfs supports only a single GDT block" >&2
		echo "workarounds:" >&2
		echo "  1) create a smaller test partition (recommended, e.g. 512 MiB or 1 GiB)" >&2
		echo "  2) use image mode on the mounted USB host filesystem" >&2
		exit 2
	fi
}

trap cleanup EXIT
trap 'on_error $LINENO' ERR

log_step "build cryexts"
make

if [ "$DEMO_MODE" = "auto" ]; then
	if [ -n "$TARGET_DEVICE" ]; then
		DEMO_MODE=raw
	elif [ -d "$USB_HOST_DIR" ]; then
		DEMO_MODE=image
	else
		echo "auto mode could not choose demo mode" >&2
		echo "either:" >&2
		echo "  1) mount your USB host filesystem and use image mode" >&2
		echo "     USB_HOST_DIR=/media/\$USER/YourUsb ./scripts/smoke_v7_0_usb_demo.sh" >&2
		echo "  2) run raw-device mode explicitly" >&2
		echo "     DEMO_MODE=raw TARGET_DEVICE=/dev/sdX1 ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK ./scripts/smoke_v7_0_usb_demo.sh" >&2
		exit 2
	fi
fi

if [ "$DEMO_MODE" = "image" ]; then
	if [ ! -d "$USB_HOST_DIR" ]; then
		echo "USB_HOST_DIR does not exist: $USB_HOST_DIR" >&2
		echo "mount your USB first, or set USB_HOST_DIR to a prepared host directory" >&2
		echo "if you want to test the partition directly, run:" >&2
		echo "  DEMO_MODE=raw TARGET_DEVICE=/dev/sdX1 ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK ./scripts/smoke_v7_0_usb_demo.sh" >&2
		exit 2
	fi
	rm -f "$IMG_PATH"
	dd if=/dev/zero of="$IMG_PATH" bs=1M count="$SIZE_MB"
	TARGET_FS=$IMG_PATH
elif [ "$DEMO_MODE" = "raw" ]; then
	if [ -z "$TARGET_DEVICE" ]; then
		echo "raw-device mode requires TARGET_DEVICE=/dev/sdX1" >&2
		exit 2
	fi
	require_raw_partition
	check_raw_device_size
	if findmnt -rn -S "$TARGET_DEVICE" >/dev/null 2>&1; then
		sudo umount "$TARGET_DEVICE"
	fi
	TARGET_FS=$TARGET_DEVICE
else
	echo "unsupported DEMO_MODE=$DEMO_MODE" >&2
	exit 2
fi

MKFS_ARGS=(-f -G -X -A -I -T -M -J -P 7 -L "$LABEL")
if [ -n "$KEY" ]; then
	MKFS_ARGS+=(-E "$KEY")
fi

log_step "mkfs"
run_mkfs
log_step "fsck after mkfs"
run_fsck

log_step "prepare large source file"
dd if=/dev/urandom of="$LARGE_SRC" bs=1M count="$LARGE_MB"

log_step "insert module"
sudo insmod cryexts.ko
log_step "prepare mountpoint"
sudo mkdir -p "$MNT"
log_step "mount cryexts"
mount_cryexts "$TARGET_FS"

log_step "mkdir demo"
sudo mkdir "$MNT/demo"
log_step "mkdir demo/diridx"
sudo mkdir "$MNT/demo/diridx"
log_step "write demo info"
printf 'v7.0 usb demo\n' | sudo tee "$MNT/demo/info.txt" >/dev/null
log_step "rename demo info"
sudo mv "$MNT/demo/info.txt" "$MNT/demo/info-renamed.txt"
log_step "create hardlink"
sudo ln "$MNT/demo/info-renamed.txt" "$MNT/demo/info.hard"
log_step "create symlink"
sudo ln -s info-renamed.txt "$MNT/demo/info.soft"
log_step "write demo note"
printf 'note=v7.0-demo\n' | sudo tee "$MNT/demo/.demo-note" >/dev/null
log_step "set xattr"
sudo python3 - "$MNT/demo/info-renamed.txt" <<'PY'
import os
import sys

os.setxattr(sys.argv[1], b"user.demo", b"baseline")
PY

log_step "populate dir index directory"
for i in $(seq 1 "$DIR_FILE_COUNT"); do
	printf 'entry-%03d\n' "$i" | sudo tee "$MNT/demo/diridx/file_$i.txt" >/dev/null
done

log_step "copy large file"
sudo cp "$LARGE_SRC" "$MNT/demo/large.bin"
log_step "create sparse file"
sudo truncate -s "$((SPARSE_SIZE_MB * 1024 * 1024))" "$MNT/demo/sparse.bin"
log_step "write sparse file markers"
printf 'head-marker\n' | sudo dd of="$MNT/demo/sparse.bin" bs=1 seek=0 conv=notrunc status=none
printf 'tail-marker\n' | sudo dd of="$MNT/demo/sparse.bin" bs=1 \
	seek="$((SPARSE_SIZE_MB * 1024 * 1024 - 12))" conv=notrunc status=none

log_step "verify first mount contents"
cmp "$LARGE_SRC" <(sudo cat "$MNT/demo/large.bin")
test "$(sudo python3 - "$MNT/demo/info-renamed.txt" <<'PY'
import os
import sys
print(os.getxattr(sys.argv[1], b"user.demo").decode(), end="")
PY
)" = "baseline"
test "$(readlink "$MNT/demo/info.soft")" = "info-renamed.txt"
test "$(stat -c %h "$MNT/demo/info.hard")" = "2"
test -f "$MNT/demo/info-renamed.txt"
test "$(sudo cat "$MNT/demo/info.soft")" = "v7.0 usb demo"

log_step "sync and unmount first mount"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

log_step "fsck after first unmount"
run_fsck

if [ -n "$KEY" ] && [ "$DEMO_MODE" = "image" ]; then
	log_step "verify encrypted image is not plaintext"
	if grep -a -q "v7.0 usb demo" "$TARGET_FS"; then
		echo "plaintext found in encrypted image" >&2
		exit 1
	fi
fi

log_step "insert module for remount"
sudo insmod cryexts.ko
log_step "remount cryexts"
mount_cryexts "$TARGET_FS"

log_step "copy file back out"
sudo cp "$MNT/demo/large.bin" "$LARGE_DST"
log_step "verify remount contents"
cmp "$LARGE_SRC" "$LARGE_DST"
test "$(sudo python3 - "$MNT/demo/info-renamed.txt" <<'PY'
import os
import sys
print(os.getxattr(sys.argv[1], b"user.demo").decode(), end="")
PY
)" = "baseline"
test "$(readlink "$MNT/demo/info.soft")" = "info-renamed.txt"
test -f "$MNT/demo/diridx/file_1.txt"
test -f "$MNT/demo/diridx/file_$DIR_FILE_COUNT.txt"
test "$(sudo cat "$MNT/demo/info.soft")" = "v7.0 usb demo"

log_step "sync and unmount second mount"
sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

log_step "fsck after second unmount"
run_fsck

trap - EXIT
echo "v7.0 usb demo smoke test passed"
