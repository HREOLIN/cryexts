#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v11_4.img}
MNT=${MNT:-/tmp/cryexts-v11_4-mnt}
LABEL=${LABEL:-v114ordered}
EXPECTED=${EXPECTED:-/tmp/cryexts-v11_4-expected.bin}
ACTUAL=${ACTUAL:-/tmp/cryexts-v11_4-actual.bin}

log_step() { echo "[v11.4] $1"; }

cleanup() {
	if mountpoint -q "$MNT"; then sudo umount "$MNT" || true; fi
	if lsmod | grep -q '^cryexts '; then sudo rmmod cryexts || true; fi
	sudo rm -f "$EXPECTED" "$ACTUAL"
}
trap cleanup EXIT

log_step "build cryexts"
make
log_step "prepare image"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=128 status=none
./mkfs.cryexts -f -G -X -A -I -T -M -J -P 7 -L "$LABEL" "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

log_step "new allocation ordered write and fsync"
python3 - "$EXPECTED" <<'PY'
from pathlib import Path
import sys

data = bytearray(b"A" * 16384)
data[4096:8192] = b"B" * 4096
data[8192:12288] = b"C" * 4096
Path(sys.argv[1]).write_bytes(data)
PY
sudo python3 - "$MNT/ordered.bin" <<'PY'
import os
import sys

data = b"A" * 4096 + b"B" * 4096 + b"C" * 4096 + b"A" * 4096
with open(sys.argv[1], "wb", buffering=0) as f:
    f.write(data)
    os.fsync(f.fileno())
PY

log_step "in-place overwrite and fsync"
sudo python3 - "$MNT/ordered.bin" <<'PY'
import os
import sys

with open(sys.argv[1], "r+b", buffering=0) as f:
    f.seek(4096)
    f.write(b"D" * 4096)
    os.fsync(f.fileno())
PY
python3 - "$EXPECTED" <<'PY'
from pathlib import Path
import sys

data = bytearray(Path(sys.argv[1]).read_bytes())
data[4096:8192] = b"D" * 4096
Path(sys.argv[1]).write_bytes(data)
PY

log_step "truncate after dirty data"
sudo python3 - "$MNT/truncate.bin" <<'PY'
import os
import sys

with open(sys.argv[1], "wb", buffering=0) as f:
    f.write(b"truncate-me" * 2048)
    os.fsync(f.fileno())
with open(sys.argv[1], "r+b", buffering=0) as f:
    f.truncate(4096)
    os.fsync(f.fileno())
PY

log_step "verify before remount"
sudo dd if="$MNT/ordered.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"
test "$(stat -c %s "$MNT/truncate.bin")" -eq 4096

log_step "remount"
sudo umount "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo dd if="$MNT/ordered.bin" of="$ACTUAL" bs=4K status=none
cmp -s "$EXPECTED" "$ACTUAL"
test "$(stat -c %s "$MNT/truncate.bin")" -eq 4096

sudo umount "$MNT"
sudo rmmod cryexts
log_step "final fsck"
./cryextsck "$IMG"
echo "v11.4 data=ordered fsync smoke test passed"
