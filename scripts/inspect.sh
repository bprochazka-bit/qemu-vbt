#!/bin/bash
# inspect.sh — send a command to a vbt-medium control socket
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Usage:
#   scripts/inspect.sh [ctl-socket] [command...]
#
# Examples:
#   scripts/inspect.sh /tmp/vbt.ctl LIST_PEERS
#   scripts/inspect.sh /tmp/vbt.ctl LIST_CONNS
#   scripts/inspect.sh                       # defaults: /tmp/vbt.ctl STATS

set -euo pipefail

CTL="${1:-/tmp/vbt.ctl}"
shift || true
CMD="${*:-STATS}"

if [ ! -S "${CTL}" ]; then
    echo "error: ${CTL} is not a socket (is vbt-medium running with -c ${CTL}?)" >&2
    exit 1
fi

if command -v socat >/dev/null 2>&1; then
    printf '%s\n' "${CMD}" | socat - "UNIX-CONNECT:${CTL}"
elif command -v nc >/dev/null 2>&1; then
    printf '%s\n' "${CMD}" | nc -U -q1 "${CTL}"
else
    echo "error: need socat or nc to talk to the control socket" >&2
    exit 1
fi
