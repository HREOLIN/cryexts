#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v4_4.img}
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
./mkfs.cryexts -f -G -X -A -P 7 -L v4xattr "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo mkdir -p "$MNT/policy_dir"
sudo python3 - "$MNT" <<'PY'
import os
import sys

mnt = sys.argv[1]
dir_path = os.path.join(mnt, "policy_dir")
file_path = os.path.join(dir_path, "child.txt")

os.setxattr(dir_path, b"user.cryexts.policy_id", b"42")
os.setxattr(dir_path, b"user.note", b"dir-tag")

with open(file_path, "wb") as fp:
    fp.write(b"v4.4-xattr-policy\n")

policy = os.getxattr(file_path, b"user.cryexts.policy_id")
if policy != b"42":
    raise SystemExit(f"unexpected inherited policy: {policy!r}")

os.setxattr(file_path, b"user.note", b"child-tag")
note = os.getxattr(file_path, b"user.note")
if note != b"child-tag":
    raise SystemExit(f"unexpected note xattr: {note!r}")

names = sorted(os.listxattr(file_path))
expected = ["user.cryexts.policy_id", "user.note"]
if names != expected:
    raise SystemExit(f"unexpected xattr names: {names!r}")
PY

sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo python3 - "$MNT" <<'PY'
import os
import sys

mnt = sys.argv[1]
file_path = os.path.join(mnt, "policy_dir", "child.txt")

policy = os.getxattr(file_path, b"user.cryexts.policy_id")
note = os.getxattr(file_path, b"user.note")
if policy != b"42":
    raise SystemExit(f"policy lost after remount: {policy!r}")
if note != b"child-tag":
    raise SystemExit(f"xattr lost after remount: {note!r}")
PY

sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

trap - EXIT
echo "v4.4 xattr/policy smoke test passed"
