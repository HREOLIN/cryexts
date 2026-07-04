#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v5_4.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
KEY=${KEY:-v54-secret}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f /tmp/cryexts-v5_4-policy.txt
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -X -A -T -P 7 -E "$KEY" -L v54policy "$IMG"
./cryextsck "$IMG"
./cryexts_policy_inspect "$IMG" | tee /tmp/cryexts-v5_4-policy.txt
grep -Eq '^policy_table_block=[1-9][0-9]*$' /tmp/cryexts-v5_4-policy.txt
grep -Eq '^default_policy=7$' /tmp/cryexts-v5_4-policy.txt

if lsmod | grep -q '^cryexts '; then
	sudo rmmod cryexts
fi
sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop,key="$KEY" -t cryexts "$IMG" "$MNT"

sudo python3 - <<'PY'
import os
import pathlib

mnt = pathlib.Path("/tmp/cryexts-mnt")
same = b"policy-aware-plaintext-block\n" * 32

dir_default = mnt / "default_dir"
dir_alt = mnt / "alt_dir"
dir_default.mkdir()
dir_alt.mkdir()

os.setxattr(dir_alt, b"user.cryexts.policy_id", b"9")

f1 = dir_default / "a.bin"
f2 = dir_alt / "b.bin"

f1.write_bytes(same)
f2.write_bytes(same)

assert f1.read_bytes() == same
assert f2.read_bytes() == same

assert os.getxattr(dir_default, b"user.cryexts.policy_id") == b"7"
assert os.getxattr(dir_alt, b"user.cryexts.policy_id") == b"9"
assert os.getxattr(f1, b"user.cryexts.policy_id") == b"7"
assert os.getxattr(f2, b"user.cryexts.policy_id") == b"9"
PY

sudo umount "$MNT"
sudo rmmod cryexts

python3 - <<'PY'
from pathlib import Path

img = Path("cryexts-v5_4.img").read_bytes()
needle = b"policy-aware-plaintext-block\n"

if needle in img:
    raise SystemExit("plaintext leaked into raw image")
PY

./cryextsck "$IMG"

trap - EXIT
echo "v5.4 policy-aware encryption smoke test passed"
