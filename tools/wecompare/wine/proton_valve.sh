#!/bin/bash
# usage: proton_valve.sh <proton-experimental|proton-11|proton-10|proton-9|proton-8|proton-7|proton-hotfix>
# Valve's Proton builds from Steam's CDN, the current build of that branch into $PROTON_DIR/<name> (run again to
# update). Files come from DepotDownloader, which writes symlinks as empty files and keeps their targets encrypted,
# so steam_manifest_links.py fixes them up with the depot key (from the python steam package in a venv).
# Only Proton Experimental is served to anonymous logins (steamcmd refuses even that one: "No subscription");
# the others need STEAM_USER=<account>, which asks for the password / Steam Guard code (run it in a terminal).
HERE=$(cd "$(dirname "$0")" && pwd)
P=${PROTON_DIR:-$HOME/.local/share/proton}
DDV=3.4.0
DD=$P/tools/depotdownloader/DepotDownloader
PY=$P/tools/steampy/bin/python
case $1 in
proton-experimental) app=1493710 ;;
proton-11) app=4628710 ;;
proton-10) app=3658110 ;;
proton-9) app=2805730 ;;
proton-8) app=2348590 ;;
proton-7) app=1887720 ;;
proton-hotfix) app=2180100 ;;
*) echo "unknown Valve build $1"; exit 1 ;;
esac
# the main depot is app id + 1, the others are optional contrib dlls
depot=$((app + 1))
if [ ! -x "$DD" ]; then
    mkdir -p "$P/tools/depotdownloader" && cd "$P/tools/depotdownloader" || exit 1
    curl -fsSLO "https://github.com/SteamRE/DepotDownloader/releases/download/DepotDownloader_$DDV/DepotDownloader-linux-x64.zip" || exit 1
    unzip -oq DepotDownloader-linux-x64.zip && chmod +x DepotDownloader && rm DepotDownloader-linux-x64.zip
fi
if [ ! -x "$PY" ]; then
    # gevent in the steam package doesn't build on 3.14 yet
    python3.13 -m venv "$P/tools/steampy" && "$P/tools/steampy/bin/pip" -q install "steam[client]" || exit 1
fi
login=()
[ -n "$STEAM_USER" ] && login=(-username "$STEAM_USER" -remember-password)
cd "$P/tools/depotdownloader" || exit 1
"$DD" -app $app -depot $depot -dir "$P/$1" -validate "${login[@]}" || exit 1
key=$("$PY" -I - $app $depot "$STEAM_USER" <<'PY'
import sys
from steam.client import SteamClient
app, depot, user = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
c = SteamClient()
if (c.cli_login(user) if user else c.anonymous_login()) != 1:
    sys.exit('steam login failed')
r = c.get_depot_key(app, depot)
if r.eresult != 1:
    sys.exit('no depot key (eresult %d)' % r.eresult)
print(r.depot_encryption_key.hex())
PY
) || exit 1
"$PY" -I "$HERE/steam_manifest_links.py" "$P/$1" "$key" || exit 1
[ -x "$P/$1/proton" ] || { echo "no proton script in $P/$1"; exit 1; }
bash "$HERE/proton.sh" env "$1" >/dev/null
echo "installed $P/$1 ($(cat "$P/$1/version"))"
