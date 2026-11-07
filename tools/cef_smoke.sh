#!/bin/bash
# CEF HTML layer smoke test — manual verification entry.
# Usage: XMEDIA_CEF_ROOT=~/Develop/vspo/cef tools/cef_smoke.sh
set -euo pipefail
: "${XMEDIA_CEF_ROOT:?set XMEDIA_CEF_ROOT to the CEF distro root}"
HELPER="${XMEDIA_CEF_HELPER:-$(cd "$(dirname "$0")/.." && pwd)/build/bin/xmedia_cef_helper}"
SMOKE="$(cd "$(dirname "$0")/.." && pwd)/build/bin/media_cef_smoke"
export XMEDIA_CEF_HELPER="$HELPER"
echo "=== CEF HTML layer smoke ==="
echo "  distro:  $XMEDIA_CEF_ROOT"
echo "  helper:  $HELPER"
"$SMOKE"
