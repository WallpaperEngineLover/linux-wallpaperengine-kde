#!/bin/bash
# bootstrap_ssh.sh <vm> <port> <layout us|de> [password] - for a VM installed from an ISO that has no ssh: serves a setup script
# from this host ($PORT, pick one nobody else uses) and runs it in a terminal that is already open and focused in the guest
# (the QEMU user network reaches the host as 10.0.2.2). The script enables sshd, adds the rig key, passwordless sudo,
# FETCH='wget -q' for a guest without curl (default 'curl -sO'); autologin is distro specific and not done here. The guest password is typed by the script's sudo (default labbalab1).
HERE=$(cd "$(dirname "$0")" && pwd)
LINUXVM=${LINUXVM:-$HOME/.local/share/linuxvm}
vm=$1 port=$2 layout=$3 pw=${4:-labbalab1}
dir=$(mktemp -d)
cat > "$dir/s" <<EOS
#!/bin/bash
echo $pw | sudo -S true 2>/dev/null
sudo systemctl enable --now sshd 2>/dev/null || sudo systemctl enable --now ssh
mkdir -p ~/.ssh; chmod 700 ~/.ssh; echo '$(cat "$LINUXVM/id_ed25519.pub")' >> ~/.ssh/authorized_keys; chmod 600 ~/.ssh/authorized_keys
echo 'lab ALL=(ALL) NOPASSWD: ALL' | sudo tee /etc/sudoers.d/lab >/dev/null
sudo chsh -s /bin/bash lab
sudo ufw allow 22 2>/dev/null; sudo firewall-cmd --add-service=ssh --permanent 2>/dev/null
echo SETUP-DONE
EOS
(cd "$dir" && exec python3 -m http.server "$port" --bind 0.0.0.0 >/dev/null 2>&1) &
server=$!
sleep 1
q() { "$HERE/qtype.sh" "$vm" "$layout" "$1"; }
q "${FETCH:-curl -sO} 10.0.2.2:$port/s"; q ret; sleep 4; q "bash s"; q ret; sleep 15
kill "$server"; rm -rf "$dir"
