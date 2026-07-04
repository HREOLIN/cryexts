#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v4_5.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-96}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -A -P 9 -L v45mvp "$IMG"
./cryextsck "$IMG"

./cryexts_journal_inject "$IMG"
echo "v4.5 recovery image injected"

if ./cryextsck "$IMG"; then
	echo "expected pre-replay cryextsck to fail on recovery image" >&2
	exit 1
fi
echo "pre-replay fsck failed as expected"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
echo "mount-time replay succeeded"

sudo mkdir -p "$MNT/pdir"
sudo python3 - "$MNT" <<'PY'
import os
import sys

mnt = sys.argv[1]
dir_path = os.path.join(mnt, "pdir")
file_path = os.path.join(dir_path, "note.txt")

os.setxattr(dir_path, b"user.cryexts.policy_id", b"55")
os.setxattr(dir_path, b"user.note", b"dir-v45")

with open(file_path, "wb") as fp:
    fp.write(b"v4.5-journal-and-xattr\n")

policy = os.getxattr(file_path, b"user.cryexts.policy_id")
note = os.getxattr(dir_path, b"user.note")
if policy != b"55":
    raise SystemExit(f"unexpected inherited policy: {policy!r}")
if note != b"dir-v45":
    raise SystemExit(f"unexpected dir note: {note!r}")
PY

sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

sudo python3 - "$IMG" <<'PY'
import struct
import sys

img = sys.argv[1]
BLOCK = 4096
SUPER_OFFSET = 1024
FEATURES_INCOMPAT_OFF = 148
STATE_OFF = 184
JOURNAL_BLOCK_OFF = 224

with open(img, "r+b") as f:
    sb = bytearray(f.read(BLOCK))
    journal_block = struct.unpack_from("<Q", sb, SUPER_OFFSET + JOURNAL_BLOCK_OFF)[0]
    state = struct.unpack_from("<I", sb, SUPER_OFFSET + STATE_OFF)[0]
    incompat = struct.unpack_from("<I", sb, SUPER_OFFSET + FEATURES_INCOMPAT_OFF)[0]
    state |= 0x00000002
    incompat |= 0x00000004
    struct.pack_into("<I", sb, SUPER_OFFSET + STATE_OFF, state)
    struct.pack_into("<I", sb, SUPER_OFFSET + FEATURES_INCOMPAT_OFF, incompat)
    f.seek(0)
    f.write(sb)

    f.seek(journal_block * BLOCK)
    hdr = bytearray(BLOCK)
    struct.pack_into("<I", hdr, 0, 0x4A4E4C31)
    struct.pack_into("<I", hdr, 4, 0)
    struct.pack_into("<I", hdr, 8, 0)
    struct.pack_into("<I", hdr, 12, 0x12345678)
    f.write(hdr)
PY

if ./cryextsck "$IMG"; then
	echo "expected broken journal header to be detected" >&2
	exit 1
fi
echo "broken journal header detected as expected"

./cryextsck --repair "$IMG"
./cryextsck "$IMG"

trap - EXIT
echo "v4.5 mvp smoke test passed"
