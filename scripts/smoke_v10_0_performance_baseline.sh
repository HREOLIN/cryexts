#!/usr/bin/env bash
set -euo pipefail

SCRIPT_TAG=${SCRIPT_TAG:-v10.0}
DEMO_MODE=${DEMO_MODE:-image}
IMG=${IMG:-cryexts-v10_0.img}
TARGET_DEVICE=${TARGET_DEVICE:-}
ACK_RAW_DEVICE=${ACK_RAW_DEVICE:-}
ALLOW_WHOLE_DISK=${ALLOW_WHOLE_DISK:-}
MNT=${MNT:-/tmp/cryexts-v10_0-mnt}
RESULTS=${RESULTS:-/tmp/cryexts-v10_0-results.txt}
SIZE_MB=${SIZE_MB:-256}
WRITE_MB=${WRITE_MB:-64}
SMALL_FILE_COUNT=${SMALL_FILE_COUNT:-512}
LABEL=${LABEL:-v100perf}
KEY=${KEY:-}

CURRENT_STEP="startup"

log_step() {
	CURRENT_STEP=$1
	echo "[$SCRIPT_TAG] $CURRENT_STEP"
}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f /tmp/cryexts-v10_0-write.bin
}

trap cleanup EXIT

require_raw_partition() {
	case "$TARGET_DEVICE" in
		/dev/sd[a-z][0-9]*|/dev/nvme[0-9]n[0-9]p[0-9]*|/dev/mmcblk[0-9]p[0-9]*|/dev/loop[0-9]*)
			;;
		/dev/sd[a-z]|/dev/nvme[0-9]n[0-9]|/dev/mmcblk[0-9])
			if [ "$ALLOW_WHOLE_DISK" != "YES" ]; then
				echo "refusing unsafe raw whole-disk target: $TARGET_DEVICE" >&2
				echo "use a partition-like device or set ALLOW_WHOLE_DISK=YES" >&2
				exit 2
			fi
			;;
		*)
			echo "refusing unsafe raw target: $TARGET_DEVICE" >&2
			exit 2
			;;
	esac

	if [ "$ACK_RAW_DEVICE" != "I_UNDERSTAND_THE_RISK" ]; then
		echo "raw-device mode requires ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK" >&2
		exit 2
	fi
}

run_mkfs() {
	local -a mkfs_args

	mkfs_args=(-f -G -X -A -I -T -M -J -P 7 -L "$LABEL")
	if [ -n "$KEY" ]; then
		mkfs_args+=(-E "$KEY")
	fi

	if [ "$DEMO_MODE" = "raw" ]; then
		sudo ./mkfs.cryexts "${mkfs_args[@]}" "$TARGET_FS"
	else
		./mkfs.cryexts "${mkfs_args[@]}" "$TARGET_FS"
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
	if [ -n "$KEY" ]; then
		if [ "$DEMO_MODE" = "image" ]; then
			sudo mount -o loop,key="$KEY" -t cryexts "$TARGET_FS" "$MNT"
		else
			sudo mount -t cryexts -o key="$KEY" "$TARGET_FS" "$MNT"
		fi
	else
		if [ "$DEMO_MODE" = "image" ]; then
			sudo mount -o loop -t cryexts "$TARGET_FS" "$MNT"
		else
			sudo mount -t cryexts "$TARGET_FS" "$MNT"
		fi
	fi
}

extract_dd_mb_s() {
	awk '
		/copied,/ {
			value = $(NF - 1) + 0
			unit = $NF
			sub(/\/s$/, "", unit)
			if (unit == "GB")
				value *= 1024
			else if (unit == "TB")
				value *= 1024 * 1024
			else if (unit == "kB")
				value /= 1024
			printf "%.2f\n", value
		}
	' | tail -n 1
}

measure_elapsed_seconds() {
	local start_ns=$1
	local end_ns=$2

	awk -v start="$start_ns" -v end="$end_ns" 'BEGIN { printf "%.6f", (end - start) / 1000000000 }'
}

metric_div() {
	local numerator=$1
	local denominator=$2

	awk -v n="$numerator" -v d="$denominator" 'BEGIN { if (d == 0) print "0.00"; else printf "%.2f", n / d }'
}

log_step "build cryexts"
make

if [ "$DEMO_MODE" = "image" ]; then
	rm -f "$IMG"
	dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
	TARGET_FS=$IMG
elif [ "$DEMO_MODE" = "raw" ]; then
	if [ -z "$TARGET_DEVICE" ]; then
		echo "raw-device mode requires TARGET_DEVICE=/dev/sdX1" >&2
		exit 2
	fi
	require_raw_partition
	if findmnt -rn -S "$TARGET_DEVICE" >/dev/null 2>&1; then
		sudo umount "$TARGET_DEVICE"
	fi
	TARGET_FS=$TARGET_DEVICE
else
	echo "unsupported DEMO_MODE=$DEMO_MODE" >&2
	exit 2
fi

log_step "mkfs"
run_mkfs
log_step "fsck after mkfs"
run_fsck

log_step "insert module"
sudo insmod cryexts.ko
log_step "prepare mountpoint"
sudo mkdir -p "$MNT"
log_step "mount cryexts"
mount_cryexts

sudo rm -f "$RESULTS"

log_step "sequential write baseline"
write_out=$(sudo dd if=/dev/zero of="$MNT/seq_write.bin" bs=1M count="$WRITE_MB" conv=fsync status=progress 2>&1)
printf '%s\n' "$write_out"
seq_write_mb_s=$(printf '%s\n' "$write_out" | extract_dd_mb_s)
test -n "$seq_write_mb_s"

log_step "sequential read baseline"
read_out=$(sudo dd if="$MNT/seq_write.bin" of=/dev/null bs=1M status=progress 2>&1)
printf '%s\n' "$read_out"
seq_read_mb_s=$(printf '%s\n' "$read_out" | extract_dd_mb_s)
test -n "$seq_read_mb_s"

log_step "small-file create baseline"
sudo mkdir -p "$MNT/smallfiles"
create_start=$(date +%s%N)
for i in $(seq 1 "$SMALL_FILE_COUNT"); do
	printf 'file-%04d\n' "$i" | sudo tee "$MNT/smallfiles/file_$i.txt" >/dev/null
done
sync
create_end=$(date +%s%N)
create_seconds=$(measure_elapsed_seconds "$create_start" "$create_end")
create_rate=$(metric_div "$SMALL_FILE_COUNT" "$create_seconds")

log_step "directory scan baseline"
scan_start=$(date +%s%N)
scan_count=$(find "$MNT/smallfiles" -maxdepth 1 -type f | wc -l | tr -d ' ')
scan_end=$(date +%s%N)
scan_seconds=$(measure_elapsed_seconds "$scan_start" "$scan_end")
scan_rate=$(metric_div "$scan_count" "$scan_seconds")

log_step "small-file unlink baseline"
unlink_start=$(date +%s%N)
sudo find "$MNT/smallfiles" -maxdepth 1 -type f -delete
sync
unlink_end=$(date +%s%N)
unlink_seconds=$(measure_elapsed_seconds "$unlink_start" "$unlink_end")
unlink_rate=$(metric_div "$SMALL_FILE_COUNT" "$unlink_seconds")

cat <<EOF | sudo tee "$RESULTS" >/dev/null
mode=$DEMO_MODE
target=$TARGET_FS
encrypted=$( [ -n "$KEY" ] && echo yes || echo no )
seq_write_MBps=$seq_write_mb_s
seq_read_MBps=$seq_read_mb_s
small_create_files_per_sec=$create_rate
small_unlink_files_per_sec=$unlink_rate
dir_scan_entries_per_sec=$scan_rate
small_file_count=$SMALL_FILE_COUNT
seq_write_size_MB=$WRITE_MB
EOF

log_step "print baseline results"
sudo cat "$RESULTS"

log_step "unmount"
sudo umount "$MNT"
log_step "remove module"
sudo rmmod cryexts
log_step "fsck after benchmark"
run_fsck

echo "v10.0 performance baseline smoke test passed"
