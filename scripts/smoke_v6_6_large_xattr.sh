#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v6_6.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
TARGET=${TARGET:-$MNT/large-xattr.bin}
INSPECT_OUT=${INSPECT_OUT:-/tmp/cryexts-v6_6-xattr.txt}
XATTR_COUNT=${XATTR_COUNT:-18}
XATTR_SIZE=${XATTR_SIZE:-220}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f "$INSPECT_OUT"
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -A -I -T -M -J -P 7 -L v66xattr "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo python3 - "$TARGET" "$XATTR_COUNT" "$XATTR_SIZE" <<'PY'
import os
import sys

target = sys.argv[1]
xattr_count = int(sys.argv[2])
xattr_size = int(sys.argv[3])

with open(target, "wb") as fp:
    fp.write(b"v6.6-large-xattr\n")

expected = {}
for i in range(xattr_count):
    name = f"user.attr{i:02d}".encode()
    fill = bytes([65 + (i % 26)])
    value = fill * xattr_size
    os.setxattr(target, name, value)
    expected[name] = value

for name, value in expected.items():
    got = os.getxattr(target, name)
    if got != value:
        raise SystemExit(f"xattr mismatch for {name!r}")

policy = os.getxattr(target, b"user.cryexts.policy_id")
if policy != b"7":
    raise SystemExit(f"unexpected inherited policy: {policy!r}")

names = sorted(os.listxattr(target))
want = sorted([name.decode() for name in expected] + ["user.cryexts.policy_id"])
if names != want:
    raise SystemExit(f"unexpected xattr names: {names!r}")
PY

inode_no=$(stat -c %i "$TARGET")

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryexts_xattr_inspect "$IMG" "$inode_no" | tee "$INSPECT_OUT"
grep -q '^xattr_present=1$' "$INSPECT_OUT"
grep -Eq '^root\.overflow_block=[1-9][0-9]*$' "$INSPECT_OUT"
grep -Eq '^overflow\.entries=[1-9][0-9]*$' "$INSPECT_OUT"

./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mount -o loop -t cryexts "$IMG" "$MNT"

sudo python3 - "$TARGET" "$XATTR_COUNT" "$XATTR_SIZE" <<'PY'
import os
import sys

target = sys.argv[1]
xattr_count = int(sys.argv[2])
xattr_size = int(sys.argv[3])

for i in range(xattr_count):
    name = f"user.attr{i:02d}".encode()
    fill = bytes([65 + (i % 26)])
    value = fill * xattr_size
    got = os.getxattr(target, name)
    if got != value:
        raise SystemExit(f"remount xattr mismatch for {name!r}")

policy = os.getxattr(target, b"user.cryexts.policy_id")
if policy != b"7":
    raise SystemExit(f"remount policy mismatch: {policy!r}")
PY

sudo sync
sudo umount "$MNT"
sudo rmmod cryexts

./cryextsck "$IMG"

trap - EXIT
echo "v6.6 large-xattr smoke test passed"
