#!/bin/bash
# Private helper: continue an IRIX installation from snapshot N with CURRENT in the drive,
# answering each "Please insert the "X" CD." request with the matching image, until inst stops
# asking.
# Snapshots are local/state/inst-N.uvstate, console logs local/runs/iN.out, the disk
# local/disks/irix.img (copied to irix-instN.img after each step). The media file names are
# the user's (assets/media); set ULTRAVIOLENT_IRIX_MEDIA for another directory.
# Usage: scripts/irix-install-discs.sh N CURRENT
set -u
cd "$(dirname "$0")/.."
M=${ULTRAVIOLENT_IRIX_MEDIA:-assets/media}
n=$1
current=$2
disc_for() {
    case "$1" in
    *FOUNDATION-1*) echo "$M/IRIX-6.5-Foundation1.iso" ;;
    *FOUNDATION-2*) echo "$M/IRIX-6.5-Foundation2.iso" ;;
    *1-of-3*|*"Installation Tools"*) echo "$M/IRIX 6.5.30 Installation Tools and Overlays (1 of 3).iso" ;;
    *2-of-3*) echo "$M/IRIX 6.5.30 Overlays (2 of 3).iso" ;;
    *3-of-3*) echo "$M/IRIX 6.5.30 Overlays (3 of 3).iso" ;;
    *) echo "" ;;
    esac
}
while :; do
    # What the guest is waiting for, from this snapshot's run and the one before it.
    text=$(cat local/runs/i$((n - 1)).out local/runs/i$n.out 2>/dev/null | tr -d '\r')
    last=$(printf '%s\n' "$text" | grep -v '^\s*$' | tail -1)
    swap=()
    case "$last" in
    *"Type control-C"*)
        want=$(printf '%s\n' "$text" | tr -d '\n' | grep -o 'Please insert the "[^"]*" CD' | tail -1)
        disc=$(disc_for "$want")
        [ -n "$disc" ] || { echo "unknown request: $want"; exit 1; }
        swap=(--console "@cdrom $disc")
        ;;
    *"Installing/removing files"*|*subsystems*) ;;
    *) echo "stopped: $last"; exit 0 ;;
    esac
    next=$((n + 1))
    echo "step $next: ${swap[*]:-(continue)}"
    build/pgo/ultraviolent --machine ip27 --prom assets/firmware/ip27prom.img \
        --load-state local/state/inst-$n.uvstate --cdrom "$current" --disk local/disks/irix.img \
        "${swap[@]}" --console "@expect Type control-C to interrupt." --console "@stop" \
        --stop-at-prompt --cycles 3000000000000 --save-state local/state/inst-$next.uvstate \
        > local/runs/i$next.out 2> local/runs/i$next.err
    tail -1 local/runs/i$next.err
    cp --sparse=always local/disks/irix.img local/disks/irix-inst$next.img
    [ ${#swap[@]} -gt 0 ] && current=${swap[1]#@cdrom }
    n=$next
done
