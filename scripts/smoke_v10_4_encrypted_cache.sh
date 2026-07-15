#!/usr/bin/env bash
set -euo pipefail

PLAIN_IMG=${PLAIN_IMG:-cryexts-v10_4-plain.img}
ENCRYPTED_IMG=${ENCRYPTED_IMG:-cryexts-v10_4-encrypted.img}
MNT=${MNT:-/tmp/cryexts-v10_4-mnt}
KEY=${KEY:-v10.4-secret}
BAD_KEY=${BAD_KEY:-v10.4-wrong}
SIZE_MB=${SIZE_MB:-128}
BENCH_MB=${BENCH_MB:-16}

EXPECTED=/tmp/cryexts-v10_4-expected.bin
PLAIN_WRITE_TIME=/tmp/cryexts-v10_4-plain-write.time
PLAIN_READ_TIME=/tmp/cryexts-v10_4-plain-read.time
ENCRYPTED_WRITE_TIME=/tmp/cryexts-v10_4-encrypted-write.time
ENCRYPTED_READ_TIME=/tmp/cryexts-v10_4-encrypted-read.time
WRONG_KEY_LOG=/tmp/cryexts-v10_4-wrong-key.log

log_step() {
	echo "[v10.4] $1"
}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	sudo rm -f "$EXPECTED" "$PLAIN_WRITE_TIME" "$PLAIN_READ_TIME" \
		"$ENCRYPTED_WRITE_TIME" "$ENCRYPTED_READ_TIME" "$WRONG_KEY_LOG"
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

benchmark_case() {
	local name=$1
	local image=$2
	local options=$3
	local write_time=$4
	local read_time=$5

	log_step "$name sequential write"
	sudo mount -o "$options" -t cryexts "$image" "$MNT"
	sudo python3 - "$MNT/bench.bin" "$BENCH_MB" >"$write_time" <<'PY'
import os
import sys
import time

path, size_mb = sys.argv[1], int(sys.argv[2])
chunk = bytes(1024 * 1024)
start = time.monotonic()
with open(path, "wb", buffering=0) as stream:
    for _ in range(size_mb):
        stream.write(chunk)
    os.fsync(stream.fileno())
print(f"{time.monotonic() - start:.9f}")
PY
	sudo umount "$MNT"

	log_step "$name remount and sequential read"
	sudo mount -o "$options" -t cryexts "$image" "$MNT"
	sudo python3 - "$MNT/bench.bin" "$BENCH_MB" >"$read_time" <<'PY'
import sys
import time

path, size_mb = sys.argv[1], int(sys.argv[2])
total = 0
start = time.monotonic()
with open(path, "rb", buffering=0) as stream:
    while chunk := stream.read(1024 * 1024):
        total += len(chunk)
elapsed = time.monotonic() - start
if total != size_mb * 1024 * 1024:
    raise SystemExit(f"benchmark read size mismatch: {total}")
print(f"{elapsed:.9f}")
PY
	sudo umount "$MNT"
}

report_rate() {
	local name=$1
	local operation=$2
	local time_file=$3

	python3 - "$name" "$operation" "$BENCH_MB" "$time_file" <<'PY'
from pathlib import Path
import sys

name, operation, size_mb, path = sys.argv[1:]
elapsed = float(Path(path).read_text().strip())
rate = float(size_mb) / max(elapsed, 0.001)
print(f"[v10.4] {name}_{operation}_mb_s={rate:.2f} elapsed_s={elapsed:.3f}")
PY
}

log_step "build cryexts"
make

log_step "create plain image"
make_image "$PLAIN_IMG" -L v104plain

log_step "create encrypted policy image"
make_image "$ENCRYPTED_IMG" -E "$KEY" -L v104encrypted

log_step "insert module"
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"

benchmark_case plain "$PLAIN_IMG" loop \
	"$PLAIN_WRITE_TIME" "$PLAIN_READ_TIME"
benchmark_case encrypted "$ENCRYPTED_IMG" "loop,key=$KEY" \
	"$ENCRYPTED_WRITE_TIME" "$ENCRYPTED_READ_TIME"

log_step "verify cached encrypted I/O with two policies"
python3 - "$EXPECTED" <<'PY'
from pathlib import Path
import sys

chunk = b"v10.4-page-cache-plaintext-policy-check\n"
Path(sys.argv[1]).write_bytes(chunk * 8192)
PY

sudo mount -o "loop,key=$KEY" -t cryexts "$ENCRYPTED_IMG" "$MNT"
sudo python3 - "$MNT" "$EXPECTED" <<'PY'
from pathlib import Path
import os
import sys

mnt = Path(sys.argv[1])
data = Path(sys.argv[2]).read_bytes()
default_dir = mnt / "policy7"
alternate_dir = mnt / "policy9"
default_dir.mkdir()
alternate_dir.mkdir()
os.setxattr(alternate_dir, b"user.cryexts.policy_id", b"9")

for directory in (default_dir, alternate_dir):
    target = directory / "cached.bin"
    with target.open("wb", buffering=0) as stream:
        for offset in range(0, len(data), 512):
            stream.write(data[offset:offset + 512])
        os.fsync(stream.fileno())
    if target.read_bytes() != data:
        raise SystemExit(f"cached encrypted read mismatch: {target}")

if os.getxattr(default_dir / "cached.bin", b"user.cryexts.policy_id") != b"7":
    raise SystemExit("default policy inheritance mismatch")
if os.getxattr(alternate_dir / "cached.bin", b"user.cryexts.policy_id") != b"9":
    raise SystemExit("alternate policy inheritance mismatch")
PY
sudo umount "$MNT"

log_step "verify encrypted files after remount"
sudo mount -o "loop,key=$KEY" -t cryexts "$ENCRYPTED_IMG" "$MNT"
cmp -s "$EXPECTED" "$MNT/policy7/cached.bin"
cmp -s "$EXPECTED" "$MNT/policy9/cached.bin"
sudo umount "$MNT"

log_step "fsck after encrypted I/O"
./cryextsck "$ENCRYPTED_IMG"

log_step "reject wrong key"
if sudo mount -o "loop,key=$BAD_KEY" -t cryexts "$ENCRYPTED_IMG" "$MNT" \
		2>"$WRONG_KEY_LOG"; then
	sudo umount "$MNT"
	echo "encrypted image mounted with the wrong key" >&2
	exit 1
fi
echo "[v10.4] wrong key rejected as expected"

log_step "remove module and inspect raw ciphertext"
sudo rmmod cryexts
python3 - "$ENCRYPTED_IMG" <<'PY'
from pathlib import Path
import sys

needle = b"v10.4-page-cache-plaintext-policy-check\n"
with Path(sys.argv[1]).open("rb") as image:
    tail = b""
    while chunk := image.read(1024 * 1024):
        data = tail + chunk
        if needle in data:
            raise SystemExit("page-cache plaintext leaked into raw image")
        tail = data[-(len(needle) - 1):]
PY

log_step "final fsck"
./cryextsck "$PLAIN_IMG"
./cryextsck "$ENCRYPTED_IMG"

report_rate plain write "$PLAIN_WRITE_TIME"
report_rate plain read "$PLAIN_READ_TIME"
report_rate encrypted write "$ENCRYPTED_WRITE_TIME"
report_rate encrypted read "$ENCRYPTED_READ_TIME"

echo "v10.4 encrypted page-cache smoke test passed"
