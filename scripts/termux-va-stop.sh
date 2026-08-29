#!/bin/sh
# termux-va-stop - stop the termux-va daemon gracefully.
#
# SIGTERM lets the daemon unlink its socket and wind down running sessions
# (it waits up to ~5s for them); we wait for the socket file to disappear
# before reporting success.

set -u

SOCK="${TERMUX_VA_SOCKET:-${TERMUX_VA_SOCKET_DIR:-${TMPDIR:-/data/data/com.termux/files/usr/tmp}/termux-va}/termux-va.sock}"

if ! pgrep -x termux-va >/dev/null 2>&1; then
    echo "termux-va is not running"
    exit 0
fi

pkill -TERM -x termux-va

i=0
while [ $i -lt 10 ]; do
    if ! pgrep -x termux-va >/dev/null 2>&1; then
        echo "termux-va stopped"
        exit 0
    fi
    sleep 1
    i=$((i + 1))
done

echo "termux-va did not exit within 10s; sending SIGKILL" >&2
pkill -KILL -x termux-va 2>/dev/null
rm -f "$SOCK"
exit 1
