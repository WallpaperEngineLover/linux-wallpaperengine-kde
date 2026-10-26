# sourced: take_slot blocks until one of MAXVMS (default 5) flock files in /tmp/lwe-vm-slots is free and keeps it in
# SLOT_FD, release_slot gives it back. Shared by every script that boots VMs, so they never run more than MAXVMS together.
take_slot() {
    local n
    mkdir -p /tmp/lwe-vm-slots
    while :; do
        for n in $(seq "${MAXVMS:-5}"); do
            exec {SLOT_FD}>"/tmp/lwe-vm-slots/slot$n" 2>/dev/null || continue
            flock -n "$SLOT_FD" && return 0
            exec {SLOT_FD}>&-
        done
        sleep 10
    done
}

release_slot() {
    [ -n "$SLOT_FD" ] && exec {SLOT_FD}>&-
    SLOT_FD=
}
