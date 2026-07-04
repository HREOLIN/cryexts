#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-v5_3.img}
MNT=${MNT:-/tmp/cryexts-mnt}
SIZE_MB=${SIZE_MB:-128}
FILE_COUNT=${FILE_COUNT:-180}
DIR=${DIR:-$MNT/bigdir}

cleanup() {
	if mountpoint -q "$MNT"; then
		sudo umount "$MNT"
	fi
	if lsmod | grep -q '^cryexts '; then
		sudo rmmod cryexts
	fi
	rm -f /tmp/cryexts-v5_3-dir-index.txt
}

trap cleanup EXIT

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f -G -I -O "$IMG"
./cryextsck "$IMG"

sudo insmod cryexts.ko
sudo mkdir -p "$MNT"
sudo mount -o loop -t cryexts "$IMG" "$MNT"
sudo mkdir -p "$DIR"

for i in $(seq 1 "$FILE_COUNT"); do
	sudo touch "$DIR/file_$i"
done

test -f "$DIR/file_1"
test -f "$DIR/file_$FILE_COUNT"
inode_no=$(stat -c %i "$DIR")

sudo umount "$MNT"
./cryexts_dir_index_inspect "$IMG" "$inode_no" | tee /tmp/cryexts-v5_3-dir-index.txt
grep -Eq '^index_block=[1-9][0-9]*$' /tmp/cryexts-v5_3-dir-index.txt
grep -Eq '^entries=[1-9][0-9]*$' /tmp/cryexts-v5_3-dir-index.txt
grep -Eq '^bucket\[[0-9]+\]=0x[0-9a-fA-F]{4}$' /tmp/cryexts-v5_3-dir-index.txt

./cryextsck "$IMG"

trap - EXIT
echo "v5.3 directory-index smoke test passed"
