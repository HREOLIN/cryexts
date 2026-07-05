#!/usr/bin/env bash
set -euo pipefail

./scripts/smoke_version6_mvp.sh
./scripts/smoke_v7_1_multi_gdt_mount.sh
./scripts/smoke_v7_2_multi_gdt_fsck.sh

echo "version7 demo smoke test passed"
