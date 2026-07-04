#!/usr/bin/env bash
set -euo pipefail

./scripts/smoke_v6_0_journal_layout.sh
./scripts/smoke_v6_1_journal_transaction.sh
./scripts/smoke_v6_2_extent_tree.sh
./scripts/smoke_v6_3_sparse_file.sh
./scripts/smoke_v6_4_allocator.sh
./scripts/smoke_v6_5_dir_index_maintenance.sh
./scripts/smoke_v6_6_large_xattr.sh

echo "version6 mvp smoke test passed"
