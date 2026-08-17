#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"

log_step() {
	echo "[v11.mvp] $1"
}

log_step "legacy v2 image layout and mount"
bash scripts/smoke_v2_0_layout.sh

log_step "journal v2 compatibility layout"
bash scripts/smoke_v6_0_journal_layout.sh

log_step "plain and encrypted page-cache regression"
bash scripts/smoke_v10_5_regression.sh

log_step "journal v3 fault-injection matrix"
bash scripts/smoke_v11_3_redo_replay.sh

log_step "data=ordered and fsync"
bash scripts/smoke_v11_4_data_ordered.sh

log_step "replay/fsck soak"
for iteration in 1 2; do
	COMMITTED_IMG="cryexts-v11_5-soak-${iteration}-committed.img" \
	UNCOMMITTED_IMG="cryexts-v11_5-soak-${iteration}-uncommitted.img" \
	PARTIAL_IMG="cryexts-v11_5-soak-${iteration}-partial.img" \
	MNT="/tmp/cryexts-v11_5-soak-${iteration}-mnt" \
	INSPECT="/tmp/cryexts-v11_5-soak-${iteration}-journal.txt" \
	FSCK_LOG="/tmp/cryexts-v11_5-soak-${iteration}-fsck.txt" \
	bash scripts/smoke_v11_3_redo_replay.sh
done

echo "version11 MVP smoke test passed"
