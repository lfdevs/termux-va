#!/usr/bin/env bash
# check-mirror.sh - verify that common/tva_protocol.h is byte-identical to
# its mirror in the Mesa tree (mesa-for-android-container, branch
# test/add-va-bridge).
#
# The mirror discipline is inherited from anland-termux: the wire protocol
# has one source of truth, and every consumer keeps a cmp-verified copy.
#
# Usage:
#   bash scripts/check-mirror.sh
#   MESA_REPO=/path/to/mesa-for-android-container bash scripts/check-mirror.sh

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MESA_REPO="${MESA_REPO:-$REPO_DIR/../mesa-for-android-container}"
MIRROR="$MESA_REPO/src/gallium/frontends/va/tva_protocol.h"

if [[ ! -f "$MIRROR" ]]; then
    echo "mirror not found: $MIRROR" >&2
    echo "set MESA_REPO to the mesa-for-android-container checkout" >&2
    exit 1
fi

if cmp -s "$REPO_DIR/common/tva_protocol.h" "$MIRROR"; then
    echo "protocol mirror OK: $MIRROR"
else
    echo "protocol mirror MISMATCH:" >&2
    echo "  source: $REPO_DIR/common/tva_protocol.h" >&2
    echo "  mirror: $MIRROR" >&2
    diff -u "$REPO_DIR/common/tva_protocol.h" "$MIRROR" || true
    exit 1
fi
