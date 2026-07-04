#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v3_1.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
PAYLOAD_A=${PAYLOAD_A:-alpha-data}
PAYLOAD_B=${PAYLOAD_B:-beta-data}

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
./mkfs.cryexts -f "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"

printf '%s\n' "$PAYLOAD_A" | sudo tee "$MNT/a.txt" >/dev/null
sudo mv "$MNT/a.txt" "$MNT/a_renamed.txt"
test ! -e "$MNT/a.txt"
test "$(cat "$MNT/a_renamed.txt")" = "$PAYLOAD_A"

sudo mkdir -p "$MNT/d1" "$MNT/d2"
sudo mv "$MNT/a_renamed.txt" "$MNT/d2/moved.txt"
test ! -e "$MNT/a_renamed.txt"
test "$(cat "$MNT/d2/moved.txt")" = "$PAYLOAD_A"

printf '%s\n' "$PAYLOAD_B" | sudo tee "$MNT/replace_src.txt" >/dev/null
printf 'old-target\n' | sudo tee "$MNT/d2/existing.txt" >/dev/null
sudo mv -f "$MNT/replace_src.txt" "$MNT/d2/existing.txt"
test ! -e "$MNT/replace_src.txt"
test "$(cat "$MNT/d2/existing.txt")" = "$PAYLOAD_B"

sudo mkdir -p "$MNT/tree" "$MNT/tree/sub" "$MNT/d3"
printf 'nested\n' | sudo tee "$MNT/tree/sub/nested.txt" >/dev/null
sudo mv "$MNT/tree/sub" "$MNT/d3/sub"
test ! -e "$MNT/tree/sub"
test -d "$MNT/d3/sub"
test "$(cat "$MNT/d3/sub/nested.txt")" = "nested"

sudo mkdir -p "$MNT/replace_dir_src" "$MNT/d4" "$MNT/d4/empty_dst"
printf 'dir-data\n' | sudo tee "$MNT/replace_dir_src/inside.txt" >/dev/null
sudo mv -T "$MNT/replace_dir_src" "$MNT/d4/empty_dst"
test ! -e "$MNT/replace_dir_src"
test -f "$MNT/d4/empty_dst/inside.txt"
test "$(cat "$MNT/d4/empty_dst/inside.txt")" = "dir-data"

sudo mkdir -p "$MNT/loopcheck/child"
if sudo mv "$MNT/loopcheck" "$MNT/loopcheck/child/loopcheck"; then
	echo "v3.1 smoke test failed: directory moved into its own subtree"
	exit 1
fi

sudo umount "$MNT"
./cryextsck "$IMG"

sudo mount -o loop -t cryexts "$IMG" "$MNT"
test "$(cat "$MNT/d2/moved.txt")" = "$PAYLOAD_A"
test "$(cat "$MNT/d2/existing.txt")" = "$PAYLOAD_B"
test "$(cat "$MNT/d3/sub/nested.txt")" = "nested"
test "$(cat "$MNT/d4/empty_dst/inside.txt")" = "dir-data"
sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
trap - EXIT

echo "v3.1 rename smoke test passed"
