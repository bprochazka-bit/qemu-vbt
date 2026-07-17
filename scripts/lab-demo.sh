#!/bin/bash
# lab-demo.sh — bring up a vbt-medium hub and (optionally) host controllers
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Starts the medium hub, and if run as root with hci_vhci available,
# attaches two host BLE controllers to it so you can experiment with
# bluetoothctl on the host immediately. Otherwise it just starts the hub
# and prints how to attach QEMU VMs.
#
# Usage:  scripts/lab-demo.sh [num-host-controllers]
# Ctrl-C to tear everything down.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOCK=/tmp/vbt.sock
CTL=/tmp/vbt.ctl
N="${1:-2}"

MEDIUM="${ROOT}/vbt-medium"
CONTROLLER="${ROOT}/vbt-controller"
[ -x "${MEDIUM}" ] || { echo "build first: make"; exit 1; }

pids=()
cleanup() {
    echo; echo "tearing down..."
    for p in "${pids[@]:-}"; do kill "${p}" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup INT TERM EXIT

echo "starting hub: ${MEDIUM} ${SOCK} -c ${CTL}"
"${MEDIUM}" "${SOCK}" -c "${CTL}" &
pids+=($!)
sleep 0.3

if [ "$(id -u)" = "0" ] && modprobe hci_vhci 2>/dev/null && [ -c /dev/vhci ]; then
    for i in $(seq 1 "${N}"); do
        echo "attaching host controller node host-${i}"
        "${CONTROLLER}" "${SOCK}" --node-id "host-${i}" &
        pids+=($!)
        sleep 0.2
    done
    echo
    echo "Host controllers are up. Try:"
    echo "  bluetoothctl        # list controllers, advertise/scan/pair"
    echo "  ${ROOT}/scripts/inspect.sh ${CTL} LIST_PEERS"
else
    echo
    echo "Hub is up on ${SOCK} (control ${CTL})."
    echo "Not root or hci_vhci unavailable, so no host controllers started."
    echo "Attach QEMU VMs with:"
    echo "  -device virtio-bluetooth-pci,medium=${SOCK},node_id=vm-a"
    echo "Inspect with:"
    echo "  ${ROOT}/scripts/inspect.sh ${CTL} LIST_PEERS"
fi

echo
echo "Running. Ctrl-C to stop."
wait
