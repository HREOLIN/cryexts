#!/usr/bin/env bash
set -euo pipefail

./scripts/smoke_v10_0_performance_baseline.sh
./scripts/smoke_v10_1_cached_read.sh
./scripts/smoke_v10_2_buffered_write.sh
./scripts/smoke_v10_3_writeback.sh
./scripts/smoke_v10_4_encrypted_cache.sh

echo "version10 demo smoke test passed"
