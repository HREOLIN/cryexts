#!/usr/bin/env bash
set -euo pipefail

IMG=${IMG:-cryexts-corrupt.img}
SIZE_MB=${SIZE_MB:-64}

make
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB"
./mkfs.cryexts -f "$IMG"
printf '\x00' | dd of="$IMG" bs=1 seek=1024 conv=notrunc status=none

if ./cryextsck "$IMG"; then
	echo "expected cryextsck to fail"
	exit 1
fi

echo "corrupt phase3 test passed"
