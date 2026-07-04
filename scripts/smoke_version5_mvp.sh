#!/usr/bin/env bash
set -euo pipefail

./scripts/smoke_v5_1_orphan_list.sh
./scripts/smoke_v5_2_extent_tree.sh
./scripts/smoke_v5_3_dir_index.sh
./scripts/smoke_v5_4_policy_crypto.sh
./scripts/smoke_v5_5_metadata_checksum.sh
./scripts/smoke_v5_6_prealloc_locality.sh

echo "version5 mvp smoke test passed"
