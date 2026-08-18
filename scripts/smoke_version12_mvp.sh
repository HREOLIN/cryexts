#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

log_step() {
	echo "[v12.mvp] $1"
}

log_step "journal ring layout baseline"
bash scripts/smoke_v12_0_journal_ring.sh

log_step "journal ring reuse"
bash scripts/smoke_v12_1_ring_reuse.sh

log_step "background checkpoint"
bash scripts/smoke_v12_2_background_checkpoint.sh

log_step "ring recovery fault injection"
bash scripts/smoke_v12_2_ring_recovery.sh

log_step "ring recovery soak"
for iteration in 1 2; do
	COMMITTED_IMG="cryexts-v12_2-soak-${iteration}-committed.img" \
	UNCOMMITTED_IMG="cryexts-v12_2-soak-${iteration}-uncommitted.img" \
	MNT="/tmp/cryexts-v12_2-soak-${iteration}-mnt" \
	INSPECT="/tmp/cryexts-v12_2-soak-${iteration}-journal.txt" \
	FSCK_LOG="/tmp/cryexts-v12_2-soak-${iteration}-fsck.txt" \
	bash scripts/smoke_v12_2_ring_recovery.sh
done

echo "version12 MVP smoke test passed"
