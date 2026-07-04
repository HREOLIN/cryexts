#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v3_4.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}

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

printf 'payload-v3.4\n' | sudo tee "$MNT/original.txt" >/dev/null
sudo ln "$MNT/original.txt" "$MNT/hard.txt"
test "$(cat "$MNT/hard.txt")" = "payload-v3.4"

orig_links=$(stat -c '%h' "$MNT/original.txt")
hard_links=$(stat -c '%h' "$MNT/hard.txt")
test "$orig_links" = "2"
test "$hard_links" = "2"

sudo rm "$MNT/original.txt"
test ! -e "$MNT/original.txt"
test "$(cat "$MNT/hard.txt")" = "payload-v3.4"
hard_links_after=$(stat -c '%h' "$MNT/hard.txt")
test "$hard_links_after" = "1"

sudo ln -s hard.txt "$MNT/soft.txt"
test -L "$MNT/soft.txt"
test "$(readlink "$MNT/soft.txt")" = "hard.txt"
test "$(cat "$MNT/soft.txt")" = "payload-v3.4"

sudo mkdir -p "$MNT/dir1"
sudo ln "$MNT/hard.txt" "$MNT/dir1/hard2.txt"
test "$(cat "$MNT/dir1/hard2.txt")" = "payload-v3.4"
test "$(stat -c '%h' "$MNT/hard.txt")" = "2"

sudo umount "$MNT"
./cryextsck "$IMG"

sudo mount -o loop -t cryexts "$IMG" "$MNT"
test "$(cat "$MNT/hard.txt")" = "payload-v3.4"
test "$(cat "$MNT/dir1/hard2.txt")" = "payload-v3.4"
test "$(readlink "$MNT/soft.txt")" = "hard.txt"
test "$(cat "$MNT/soft.txt")" = "payload-v3.4"
test "$(stat -c '%h' "$MNT/hard.txt")" = "2"

sudo rm "$MNT/dir1/hard2.txt"
test "$(stat -c '%h' "$MNT/hard.txt")" = "1"
sudo rm "$MNT/soft.txt"
test ! -e "$MNT/soft.txt"

sudo umount "$MNT"
sudo rmmod cryexts
./cryextsck "$IMG"
trap - EXIT

echo "v3.4 links smoke test passed"
