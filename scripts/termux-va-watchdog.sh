#!/bin/sh
# termux-va-watchdog - keep the termux-va daemon healthy.
#
# A plain-Termux (root-free) port of the upstream ksu-module watchdog idea:
# every interval, run tva-probe against the endpoint - a REAL check that
# performs a connect, a handshake, feeds an embedded 96x96 H.264 stream and
# requires at least one returned frame.  Process liveness alone cannot catch
# the "alive but serving nothing" failure class.
#
# Exit-code branching (tva-probe):
#   0 healthy            -> reset the failure counter
#   1 endpoint missing   -> count as failure (daemon down / socket gone)
#   2 handshake rejected -> count as failure (alive but not serving)
#   7 inode mismatch     -> mount/configuration problem; WARN ONLY, do not
#                           restart (restarting the daemon cannot fix it)
#   8 handshake ok, 0 frames -> count as failure (the sneaky class)
#
# After MAX_FAILS consecutive failures the daemon is restarted through
# termux-va-start; after COOLDOWN many restarts without a healthy probe the
# watchdog stops restarting (prevents crash loops) and waits for manual
# intervention.
#
# Environment:
#   TERMUX_VA_WD_INTERVAL   probe interval seconds   (default 5)
#   TERMUX_VA_WD_MAX_FAILS  failures before restart  (default 5)
#   TERMUX_VA_WD_COOLDOWN   restarts before pausing  (default 10)
#   TERMUX_VA_WD_GRACE      seconds before the first restart is allowed
#                           (default 45; gives the daemon time to start)

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

command -v tva-probe >/dev/null 2>&1 && PROBE=tva-probe || PROBE="$SCRIPT_DIR/../tools/tva-probe"
command -v termux-va-start >/dev/null 2>&1 && START=termux-va-start || START="$SCRIPT_DIR/termux-va-start.sh"

SOCK="${TERMUX_VA_SOCKET:-${TERMUX_VA_SOCKET_DIR:-${TMPDIR:-/data/data/com.termux/files/usr/tmp}/termux-va}/termux-va.sock}"
SOCK_DIR="$(dirname "$SOCK")"
LOG="${TERMUX_VA_LOG:-$SOCK_DIR/termux-va-watchdog.log}"

INTERVAL="${TERMUX_VA_WD_INTERVAL:-5}"
MAX_FAILS="${TERMUX_VA_WD_MAX_FAILS:-5}"
COOLDOWN="${TERMUX_VA_WD_COOLDOWN:-10}"
GRACE="${TERMUX_VA_WD_GRACE:-45}"

# Single-instance guard via an atomic mkdir (no flock dependency).
# The lock lives inside the socket directory on purpose: Termux wipes
# $TMPDIR when its service is destroyed, and a wiped lock is harmless.
LOCKDIR="$SOCK_DIR/watchdog.lock"
if ! mkdir "$LOCKDIR" 2>/dev/null; then
    echo "another watchdog appears to be running ($LOCKDIR), exiting" >&2
    exit 1
fi
trap 'rmdir "$LOCKDIR" 2>/dev/null' EXIT INT TERM

mkdir -p "$SOCK_DIR"

fail_count=0
restarts=0
start_ts=$(date +%s)

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') watchdog: $*" >>"$LOG"
}

log "watchdog started (interval=${INTERVAL}s max_fails=${MAX_FAILS} cooldown=${COOLDOWN} grace=${GRACE}s endpoint=$SOCK)"

while :; do
    "$PROBE" "$SOCK" 3000 >/dev/null 2>&1
    rc=$?

    case $rc in
    0)
        if [ "$fail_count" -gt 0 ] || [ "$restarts" -gt 0 ]; then
            log "healthy again (frames decoded), failures=$fail_count restarts=$restarts"
        fi
        fail_count=0
        restarts=0
        start_ts=$(date +%s)
        ;;
    7)
        log "WARN: endpoint inode mismatch (rc=7) - mount/configuration problem," \
            "not restarting the daemon; fix the mount so both sides stat the same socket"
        ;;
    1|2|8)
        fail_count=$((fail_count + 1))
        case $rc in
        1) reason="endpoint missing/connect failed" ;;
        2) reason="handshake rejected (alive but not serving)" ;;
        8) reason="handshake ok but 0 frames (not decoding)" ;;
        esac
        log "probe failure #$fail_count/$MAX_FAILS: $reason"

        if [ "$fail_count" -ge "$MAX_FAILS" ]; then
            now=$(date +%s)
            if [ $((now - start_ts)) -lt "$GRACE" ]; then
                log "grace period not elapsed, skipping restart"
            elif [ "$restarts" -ge "$COOLDOWN" ]; then
                log "restart limit ($COOLDOWN) reached without recovery," \
                    "paused - manual intervention required"
                sleep 300
            else
                log "restarting termux-va (restart #$((restarts + 1)))"
                if "$START" >>"$LOG" 2>&1; then
                    log "restart completed, re-probing"
                else
                    log "restart script failed"
                fi
                restarts=$((restarts + 1))
                fail_count=0
                start_ts=$(date +%s)
            fi
        fi
        ;;
    3)
        log "probe usage error (rc=3); check the tva-probe installation"
        ;;
    *)
        log "unknown probe exit code $rc, counted as failure"
        fail_count=$((fail_count + 1))
        ;;
    esac

    sleep "$INTERVAL"
done
