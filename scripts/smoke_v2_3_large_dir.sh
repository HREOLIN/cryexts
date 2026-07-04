#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v2_3.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-64}
COUNT=${COUNT:-200}

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

sudo mkdir "$MNT/bigdir"
for i in $(seq 1 "$COUNT"); do
	sudo touch "$MNT/bigdir/file_$i"
done

test "$(ls "$MNT/bigdir" | wc -l)" = "$COUNT"

for i in $(seq 1 "$COUNT"); do
	sudo rm "$MNT/bigdir/file_$i"
done
sudo rmdir "$MNT/bigdir"

sudo umount "$MNT"
./cryextsck "$IMG"
sudo rmmod cryexts
trap - EXIT

echo "v2.3 large-directory smoke test passed"
