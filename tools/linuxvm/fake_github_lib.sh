# fake_github_lib.sh - starts fake_github.py for a VM (sourced by clean_install_check.sh and update_check.sh, which define HERE and sshv).
# The apps ask https://api.github.com, so the VM resolves it to the host (10.0.2.2) and the port is 443. With nft or iptables in the guest that
# 443 is redirected to a port of the VM's own, so several VMs can run these checks at the same time; without (or when the redirect does not
# reach our server) the old way is used: port 443 behind a host wide lock.
# fgh_start <vm> <work dir> <fake_github.py args...> sets $fake (pid) and $fgh_port; the work dir holds certs/ and gets fake.log.

fgh_redirect() {
    sshv "sudo sh -c 'if command -v nft >/dev/null 2>&1; then nft add table ip lwefgh && nft \"add chain ip lwefgh out { type nat hook output priority -100 ; }\" && nft add rule ip lwefgh out ip daddr 10.0.2.2 tcp dport 443 dnat to 10.0.2.2:$1; elif command -v iptables >/dev/null 2>&1; then iptables -t nat -A OUTPUT -d 10.0.2.2 -p tcp --dport 443 -j DNAT --to-destination 10.0.2.2:$1; else exit 1; fi'" >/dev/null 2>&1
}

fgh_unredirect() {
    sshv "sudo sh -c 'nft delete table ip lwefgh 2>/dev/null; iptables -t nat -F OUTPUT 2>/dev/null; true'" >/dev/null 2>&1
}

fgh_start() {
    local vm=$1 work=$2
    shift 2
    fgh_port=$((30000 + $(cat "$HOME/.local/share/linuxvm/vms/$vm/port") % 10000))
    if fgh_redirect "$fgh_port"; then
        python3 "$HERE/fake_github.py" "$work/certs" "$@" --port "$fgh_port" > "$work/fake.log" 2>&1 &
        fake=$!
        sleep 3
        # a request from the guest to the host's 443 has to show up in our own log
        sshv "python3 -c \"import ssl, urllib.request as u; u.urlopen('https://10.0.2.2/repos/x/y/releases/latest', context=ssl._create_unverified_context(), timeout=8)\"" >/dev/null 2>&1
        if kill -0 $fake 2>/dev/null && grep -q GET "$work/fake.log"; then
            echo "     fake api.github.com on host port $fgh_port (redirected in the guest)"
            return 0
        fi
        kill $fake 2>/dev/null
        fgh_unredirect
    fi
    exec 9>"${TMPDIR:-/tmp}/lwe-fake-github.lock"
    flock 9
    fgh_port=443
    python3 "$HERE/fake_github.py" "$work/certs" "$@" --port 443 > "$work/fake.log" 2>&1 &
    fake=$!
    echo "     fake api.github.com on host port 443 (host wide lock)"
}
