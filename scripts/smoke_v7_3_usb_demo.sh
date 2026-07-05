#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

SCRIPT_TAG=${SCRIPT_TAG:-v7.3}
DEMO_TEXT=${DEMO_TEXT:-v7.3 usb demo}
ENTRY_SCRIPT=${ENTRY_SCRIPT:-./scripts/smoke_v7_3_usb_demo.sh}
IMG_NAME=${IMG_NAME:-cryexts-v7_3-demo.img}
LABEL=${LABEL:-v73demo}
SIZE_MB=${SIZE_MB:-1024}
DIR_FILE_COUNT=${DIR_FILE_COUNT:-128}

export SCRIPT_TAG DEMO_TEXT ENTRY_SCRIPT IMG_NAME LABEL SIZE_MB DIR_FILE_COUNT

exec "$SCRIPT_DIR/smoke_v7_0_usb_demo.sh"
