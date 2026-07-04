#!/usr/bin/env bash
set -euo pipefail

./scripts/smoke_version6_mvp.sh
./scripts/smoke_v7_0_usb_demo.sh

echo "version7 demo smoke test passed"
