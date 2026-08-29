#!/bin/sh
# termux-va-start - start the termux-va daemon (reuse if already healthy).
#
# Modeled on the anland-termux lifecycle scripts: check whether a live
# daemon is already serving the endpoint, kill stale instances otherwise,
# start a fresh one in the background, wait for the socket, then run one
# real health probe (connect + handshake + decode) before reporting success.
#
# Environment:
#   TERMUX_VA_SOCKET / TERMUX_VA_SOCKET_DIR   endpoint overrides (see README)
#   TERMUX_VA_LOG                             log file (default: inside the
#                                             socket directory)
#   TERMUX_VA_START_WAIT                      startup wait in seconds (default 10)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

command -v tva-probe >/dev/null 2>&1 && PROBE=tva-probe || PROBE="$SCRIPT_DIR/../tools/tva-probe"

# Endpoint resolution mirrors the daemon (without the adb-TMPDIR corner
# case: Termux always sets TMPDIR properly).
SOCK="${TERMUX_VA_SOCKET:-${TERMUX_VA_SOCKET_DIR:-${TMPDIR:-/data/data/com.termux/files/usr/tmp}/termux-va}/termux-va.sock}"
SOCK_DIR="$(dirname "$SOCK")"
LOG="${TERMUX_VA_LOG:-$SOCK_DIR/termux-va.log}"
WAIT="${TERMUX_VA_START_WAIT:-10}"

probe() {
    [ -S "$SOCK" ] || return 1
    "$PROBE" "$SOCK" 3000 >/dev/null 2>&1
}

# 1. Healthy instance already serving?  Reuse it.
if probe; then
    echo "termux-va already serving $SOCK, reusing"
    exit 0
fi

# 2. Not serving: stop anything stale, then start fresh.
pkill -x termux-va 2>/dev/null
sleep 1

mkdir -p "$SOCK_DIR"

nohup termux-va >>"$LOG" 2>&1 &
echo "termux-va starting, log: $LOG"

# 3. Wait for the listening socket (the daemon prints "listening on <path>"
#    into the log when ready).
i=0
while [ $i -lt $WAIT ]; do
    if [ -S "$SOCK" ]; then
        sleep 1
        if probe; then
            echo "termux-va is ready: $SOCK"
            exit 0
        fi
    fi
    i=$((i + 1))
    sleep 1
done

echo "termux-va failed to become ready within ${WAIT}s; last log lines:" >&2
tail -n 20 "$LOG" >&2 2>/dev/null
exit 1
