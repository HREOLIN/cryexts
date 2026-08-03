#!/usr/bin/env bash
set -euo pipefail

PLAIN_IMG=${PLAIN_IMG:-cryexts-v10_5-plain.img}
ENCRYPTED_IMG=${ENCRYPTED_IMG:-cryexts-v10_5-encrypted.img}
MNT=${MNT:-/tmp/cryexts-v10_5-mnt}
SIZE_MB=${SIZE_MB:-128}
BENCH_MB=${BENCH_MB:-16}
KEY=${KEY:-v10.5-secret}
BAD_KEY=${BAD_KEY:-v10.5-wrong}

EXPECTED=/tmp/cryexts-v10_5-expected.bin
SMALL_EXPECTED=/tmp/cryexts-v10_5-small-expected.bin
ACTUAL=/tmp/cryexts-v10_5-actual.bin

log_step() {
	echo "[v10.5] $1"
}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT" || true
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts || true
	fi
	sudo rm -f "$EXPECTED" "$SMALL_EXPECTED" "$ACTUAL"
}

trap cleanup EXIT

make_image() {
	local image=$1
	shift

	rm -f "$image"
	dd if=/dev/zero of="$image" bs=1M count="$SIZE_MB" status=none
	./mkfs.cryexts -f -G -X -A -I -T -M -J -P 7 "$@" "$image"
	./cryextsck "$image"
}

write_expected() {
	python3 - "$EXPECTED" "$BENCH_MB" <<'PY'
from pathlib import Path
import sys

path, size_mb = sys.argv[1], int(sys.argv[2])
pattern = bytes(i % 251 for i in range(4096))
with open(path, "wb") as stream:
    for _ in range(size_mb * 256):
        stream.write(pattern)
PY

	python3 - "$SMALL_EXPECTED" <<'PY'
from pathlib import Path
import sys

marker = b"v10.5-page-cache-regression-marker\n"
Path(sys.argv[1]).write_bytes(marker * 4096)
PY
}

write_rate() {
	local label=$1
	local path=$2
	local size_mb=$3

	sudo python3 - "$label" "$path" "$size_mb" <<'PY'
import os
import sys
import time

label, path, size_mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
total = 0
start = time.monotonic()
with open(path, "wb", buffering=0) as stream:
    pattern = bytes(i % 251 for i in range(4096))
    for _ in range(size_mb * 256):
        stream.write(pattern)
        total += len(pattern)
    stream.flush()
    os.fsync(stream.fileno())
elapsed = time.monotonic() - start
print(f"[v10.5] {label}_write_mb_s={total / 1048576 / max(elapsed, 0.001):.2f} elapsed_s={elapsed:.3f}")
PY
}

read_rate() {
	local label=$1
	local path=$2
	local size_mb=$3

	sudo python3 - "$label" "$path" "$size_mb" <<'PY'
import sys
import time

label, path, size_mb = sys.argv[1], sys.argv[2], int(sys.argv[3])
total = 0
start = time.monotonic()
with open(path, "rb", buffering=0) as stream:
    while chunk := stream.read(1024 * 1024):
        total += len(chunk)
expected = size_mb * 1048576
if total != expected:
    raise SystemExit(f"read size mismatch: {total} != {expected}")
elapsed = time.monotonic() - start
print(f"[v10.5] {label}_read_mb_s={total / 1048576 / max(elapsed, 0.001):.2f} elapsed_s={elapsed:.3f}")
PY
}

compare_file() {
	local source=$1

	sudo dd if="$source" of="$ACTUAL" bs=1M status=none
	cmp -s "$ACTUAL" "$EXPECTED"
}

compare_small_file() {
	sudo dd if="$1" of="$ACTUAL" bs=4K status=none
	cmp -s "$ACTUAL" "$SMALL_EXPECTED"
}

run_case() {
	local label=$1
	local image=$2
	local options=$3

	log_step "$label mount"
	sudo mount -o "$options" -t cryexts "$image" "$MNT"

	log_step "$label sequential write"
	write_rate "$label" "$MNT/bench.bin" "$BENCH_MB"

	log_step "$label buffered small writes"
	sudo python3 - "$MNT/small.bin" <<'PY'
import os
import sys

marker = b"v10.5-page-cache-regression-marker\n"
with open(sys.argv[1], "wb", buffering=0) as stream:
    for _ in range(4096):
        stream.write(marker)
    os.fsync(stream.fileno())
PY

	log_step "$label cached read verification"
	compare_small_file "$MNT/small.bin"

	log_step "$label remount verification"
	sudo umount "$MNT"
	sudo mount -o "$options" -t cryexts "$image" "$MNT"
	compare_file "$MNT/bench.bin"
	compare_small_file "$MNT/small.bin"

	log_step "$label cold and cached read rate"
	read_rate "$label" "$MNT/bench.bin" "$BENCH_MB"
	read_rate "${label}_cached" "$MNT/bench.bin" "$BENCH_MB"

	sudo umount "$MNT"
}

log_step "build cryexts"
make

log_step "prepare expected data"
write_expected

log_step "create plain image"
make_image "$PLAIN_IMG" -L v105plain

log_step "create encrypted image"
make_image "$ENCRYPTED_IMG" -E "$KEY" -L v105encrypted

log_step "insert module"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"

run_case plain "$PLAIN_IMG" loop
run_case encrypted "$ENCRYPTED_IMG" "loop,key=$KEY"

log_step "wrong key rejection"
if sudo mount -o "loop,key=$BAD_KEY" -t cryexts "$ENCRYPTED_IMG" "$MNT"; then
	sudo umount "$MNT"
	echo "encrypted image mounted with the wrong key" >&2
	exit 1
fi
echo "[v10.5] wrong key rejected as expected"

log_step "remove module and inspect encrypted image"
sudo rmmod cryexts
python3 - "$ENCRYPTED_IMG" "$SMALL_EXPECTED" <<'PY'
from pathlib import Path
import sys

image, expected = sys.argv[1], Path(sys.argv[2]).read_bytes()
with Path(image).open("rb") as stream:
    tail = b""
    while chunk := stream.read(1024 * 1024):
        data = tail + chunk
        if expected in data:
            raise SystemExit("page-cache plaintext leaked into encrypted image")
        tail = data[-(len(expected) - 1):]
PY

log_step "final fsck"
./cryextsck "$PLAIN_IMG"
./cryextsck "$ENCRYPTED_IMG"

echo "v10.5 page-cache/writeback regression smoke test passed"
