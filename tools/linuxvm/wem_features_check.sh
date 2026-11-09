#!/bin/bash
# wem_features_check.sh <vm> <we_manager dist/linux-unpacked | .deb | flatpak:<bundle.flatpak | installed>> [screen] - the
# we_manager features that decide what is on the screen, driven through the real app over DevTools (cdp.py): saved profiles,
# schedule rules (run now and a time trigger that fires by itself), application rules (a program runs, another app plays
# sound, revert afterwards), split screens, daytime playlists following the clock, copy and move between library folders,
# custom thumbnails, the audio input device list and the graphics notice. With flatpak: the same inside the sandbox
# (engines, host programs and the library folders go through flatpak-spawn and the sandbox's filesystem).
# Not covered here (needs a Steam client): Verify files, Repair, backup of workshop items. Fonts need a file dialog.
# Prints one PASS|FAIL|SKIP line per check. WEM_LAUNCH and WEM_FLAGS are added to the start line like in the other we_manager scripts.
HERE=$(cd "$(dirname "$0")" && pwd)
vm=$1 app=$2 screen=${3:-Virtual-1}
sshv() { "$HERE/linuxvm.sh" ssh "$vm" "$@" </dev/null; }
status=0
pass() { echo "PASS $1"; }
fail() { echo "FAIL $1"; status=1; }
skip() { echo "SKIP $1"; }
CDP_TIMEOUT=${CDP_TIMEOUT:-180}
. "$HERE/wem_lib.sh"

A=1120596121 B=1111948150 W=1078208425
LOCAL_A=900000011 LOCAL_B=900000012
FPAPP=io.github.WallpaperEngineLover.we_manager

sshv 'mkdir -p ~/wem; pkill -f "[w]e-manager" 2>/dev/null; pkill -x lwe-ruletest 2>/dev/null; pkill -x paplay 2>/dev/null; true'
wem_wait_gone
if [[ $app == flatpak:* ]]; then
    bundle=${app#flatpak:}
    sshv 'mkdir -p ~/wem'
    "$HERE/linuxvm.sh" scp "$vm" "$HERE/cdp.py" wem/cdp.py
    if [ "$bundle" != installed ]; then
        "$HERE/linuxvm.sh" scp "$vm" "$bundle" wem/app.flatpak
        sshv "flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo; flatpak --user install -y --noninteractive --reinstall ~/wem/app.flatpak >/tmp/fp-install.log 2>&1" \
            && pass "the bundle installs" || { fail "bundle install: $(sshv 'tail -3 /tmp/fp-install.log')"; exit 1; }
    fi
    wem_start_line="${WEM_LAUNCH:-} flatpak run $FPAPP ${WEM_FLAGS:-}"
    mode="inside the Flatpak"
else
    wem_install || exit 1
    wem_engine_fallback
    mode="native"
fi
wem_steam_lib $A $B $W
wem_run || exit 1
pass "we_manager starts ($mode)"

HOME_DIR=$(sshv 'echo $HOME')
LIBS=$HOME_DIR/lwe-features-lib
LIB_A=$LIBS/a LIB_B=$LIBS/b

engine_args() { sshv 'pgrep -af "/[l]inux-wallpaperengine(\.bin)? "'; }
assigned() { cdp "window.electronAPI.wallpaper.getAssignments().then(a => { const x = a.find(e => e.screen === '$screen'); return x ? x.wallpaperId : '' })" | tr -d '"'; }
wait_assigned() {  # <wallpaper id> <seconds>
    local end=$((SECONDS + $2))
    while [ $SECONDS -lt $end ]; do [ "$(assigned)" = "$1" ] && return 0; sleep 2; done
    return 1
}
apply() { cdp "window.electronAPI.wallpaper.apply({wallpaperId: '$1', screen: '$screen'})" >/dev/null; sleep 6; }
engine_runs() { [ -n "$(engine_args | grep -- "wp-$1\|/$1")" ]; }
cleanup() {
    cdp 'window.electronAPI.lwe.killAll()' >/dev/null
    cdp "(async () => { const api = window.electronAPI;
      for (const r of await api.schedule.getAll()) if (r.name.startsWith('wem-')) await api.schedule.delete(r.id);
      for (const r of await api.appRules.getAll()) if (r.name.startsWith('wem-')) await api.appRules.delete(r.id);
      for (const p of await api.profiles.getAll()) if (p.name.startsWith('wem-')) await api.profiles.delete(p.id);
      for (const p of await api.playlist.getAll()) if (p.title.startsWith('wem-')) await api.playlist.delete(p.id);
      await api.wallpaper.setSplit('$screen', null);
      await api.config.setExtraLibraries([]); await api.config.setDisplayMode('shared'); })()" >/dev/null
    sshv 'pkill -x lwe-ruletest; pkill -x paplay; pkill -f "[w]e-manager"; rm -rf ~/lwe-features-lib /tmp/lwe-ruletest /tmp/lwe-features-silence.wav; true'
}

cdp "window.electronAPI.config.setLanguage('en')" >/dev/null
cdp "window.electronAPI.config.setDisplayMode('per-screen')" >/dev/null
cdp "window.electronAPI.config.setWorkshopPath('$HOME_DIR/.steam/steam/steamapps/workshop/content/431960')" >/dev/null
for _ in 1 2 3 4 5 6; do
    scanned=$(cdp 'window.electronAPI.library.scan()' | tr '\n' ' ')
    have=$(cdp "window.electronAPI.library.getAll().then(a => [$A, $B].filter(id => a.some(w => w.id === String(id))).length)")
    [ "$have" = 2 ] && break
    sleep 5
done
if [ "$have" = 2 ]; then pass "the test wallpapers are in the library"; else
    fail "library lacks the test wallpapers ($have of 2), last scan: $scanned, workshop path: $(cdp 'window.electronAPI.config.get().then(c => c.workshopPath)' | tr -d '\n')"
    cleanup; exit 1
fi

# profiles
apply $A
out=$(cdp "window.electronAPI.profiles.capture('wem-profile').then(p => p.id + ' ' + JSON.stringify(p.screens))")
pid=$(echo "$out" | tr -d '"' | cut -d' ' -f1)
[[ $out == *"$A"* ]] && pass "a profile captures what is on the screen" || fail "profile capture: $out"
apply $B
[ "$(assigned)" = $B ] && pass "another wallpaper replaces it" || fail "apply $B: screen shows $(assigned)"
cdp "window.electronAPI.profiles.apply('$pid')" >/dev/null
wait_assigned $A 30 && pass "applying the profile brings the first wallpaper back" || fail "profile apply: screen shows $(assigned)"
engine_runs $A && pass "the engine runs the profile's wallpaper" || fail "no engine for $A after the profile: $(engine_args | cut -c1-200)"

# schedule: run now, a profile as action, a time trigger firing by itself
rule=$(cdp "window.electronAPI.schedule.save({name: 'wem-run', enabled: true, trigger: {type: 'time', time: '03:33', days: []}, action: {type: 'wallpaper', wallpaperId: '$B'}, screen: '$screen'}).then(r => r.id)" | tr -d '"')
cdp "window.electronAPI.schedule.run('$rule')" >/dev/null
wait_assigned $B 30 && pass "schedule: run now applies the rule's wallpaper" || fail "schedule run now: screen shows $(assigned)"
prule=$(cdp "window.electronAPI.schedule.save({name: 'wem-profile-rule', enabled: true, trigger: {type: 'time', time: '03:34', days: []}, action: {type: 'profile', profileId: '$pid'}, screen: '*'}).then(r => r.id)" | tr -d '"')
cdp "window.electronAPI.schedule.run('$prule')" >/dev/null
wait_assigned $A 30 && pass "schedule: a rule can load a profile" || fail "schedule profile action: screen shows $(assigned)"
cdp "window.electronAPI.schedule.delete('$prule')" >/dev/null
at=$(sshv "date -d '+2 minutes' +%H:%M")
cdp "window.electronAPI.schedule.save({id: '$rule', name: 'wem-run', enabled: true, trigger: {type: 'time', time: '$at', days: []}, action: {type: 'wallpaper', wallpaperId: '$B'}, screen: '$screen'})" >/dev/null
[ "$(assigned)" = $A ] || apply $A
if wait_assigned $B 200; then pass "schedule: a time trigger set for $at fires by itself"; else fail "schedule: nothing happened at $at, screen shows $(assigned)"; fi
cdp "window.electronAPI.schedule.delete('$rule')" >/dev/null

# application rules
sshv 'cp "$(command -v sleep)" /tmp/lwe-ruletest'
apply $A
arule=$(cdp "window.electronAPI.appRules.save({name: 'wem-process', enabled: true, condition: {type: 'process', names: ['lwe-ruletest']}, action: {type: 'wallpaper', wallpaperId: '$B'}, screen: '$screen', revert: true}).then(r => r.id)" | tr -d '"')
sleep 8
[ "$(assigned)" = $A ] && pass "app rule: nothing changes while the program is not running" || fail "app rule fired without the program: $(assigned)"
sshv ". ~/sessenv; (${WEM_LAUNCH:-} setsid nohup /tmp/lwe-ruletest 3000 >/dev/null 2>&1 &); sleep 2"
if wait_assigned $B 45; then
    pass "app rule: starting the program shows the rule's wallpaper"
    [[ $(cdp 'window.electronAPI.appRules.active()') == *"$arule"* ]] && pass "app rule: it is listed as active" || fail "app rule not listed as active"
else
    fail "app rule: program running, screen shows $(assigned)"
fi
sshv 'pkill -x lwe-ruletest; true'
if wait_assigned $A 45; then pass "app rule: the previous wallpaper comes back when the program ends"; else fail "app rule did not revert, screen shows $(assigned)"; fi
cdp "window.electronAPI.appRules.delete('$arule')" >/dev/null

if [ "$(cdp 'window.electronAPI.appRules.audioAvailable()')" = true ] && sshv 'command -v paplay >/dev/null && pactl info >/dev/null 2>&1'; then
    sshv "python3 -c \"
import wave
w = wave.open('/tmp/lwe-features-silence.wav', 'w'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
w.writeframes(b'\0' * 44100 * 4 * 60); w.close()\""
    apply $A
    srule=$(cdp "window.electronAPI.appRules.save({name: 'wem-audio', enabled: true, condition: {type: 'audio'}, action: {type: 'wallpaper', wallpaperId: '$B'}, screen: '$screen', revert: true}).then(r => r.id)" | tr -d '"')
    sleep 8
    sshv ". ~/sessenv; (${WEM_LAUNCH:-} setsid nohup paplay /tmp/lwe-features-silence.wav >/dev/null 2>&1 &); sleep 2"
    wait_assigned $B 45 && pass "app rule: another app's sound shows the rule's wallpaper" || fail "audio rule: sound playing, screen shows $(assigned)"
    sshv 'pkill -x paplay; true'
    wait_assigned $A 60 && pass "app rule: back to the previous wallpaper after the sound stops" || fail "audio rule did not revert: $(assigned)"
    cdp "window.electronAPI.appRules.delete('$srule')" >/dev/null
else
    skip "app rule on sound: no pulse tools or server in this guest"
fi

# split screen (per-screen display mode)
cdp "window.electronAPI.wallpaper.setSplit('$screen', {layout: 'equal', slots: [{wallpaperId: '$A'}, {wallpaperId: '$B'}]})" >/dev/null
cdp "window.electronAPI.wallpaper.showSplit('$screen')" >/dev/null
sleep 20
slots=$(engine_args | grep -o -- '--screen-slot' | wc -l)
[ "$slots" -ge 2 ] && pass "split screen: one engine with $slots slots" || fail "split screen: $slots --screen-slot in $(engine_args | cut -c1-300)"
[ "$(sshv 'pgrep -fc "/[l]inux-wallpaperengine(\.bin)? "')" -ge 1 ] && pass "split screen: the engine stays up" || fail "split screen: no engine running"
apply $A
[ "$(engine_args | grep -c -- '--screen-slot')" = 0 ] && pass "split screen: a normal wallpaper replaces the split" || fail "the split is still running: $(engine_args | cut -c1-300)"
cdp "window.electronAPI.wallpaper.setSplit('$screen', null)" >/dev/null

# playlist following the clock (daytime)
now=$(sshv 'echo $((10#$(date +%H) * 60 + 10#$(date +%M)))')
cdp "(async () => { const api = window.electronAPI; const p = await api.playlist.create('wem-daytime');
  await api.playlist.addItems(p.id, ['$A', '$B']);
  await api.playlist.updateSettings(p.id, {mode: 'daytime', sortBy: 'manual', randomize: false}); return p.id })()" > /dev/null
plid=$(cdp "window.electronAPI.playlist.getAll().then(a => a.find(p => p.title === 'wem-daytime').id)" | tr -d '"')
if [ "$now" -lt 1380 ]; then
    cdp "window.electronAPI.playlist.updateItem('$plid', '$A', {endMinute: $((now + 30))})" >/dev/null
    cdp "window.electronAPI.playlist.start('$plid', '$screen')" >/dev/null
    wait_assigned $A 40 && pass "daytime playlist: before the first item's end that item is shown" || fail "daytime playlist (before end): $(assigned)"
fi
if [ "$now" -ge 30 ]; then
    cdp "window.electronAPI.playlist.updateItem('$plid', '$A', {endMinute: $((now - 30))})" >/dev/null
    cdp "window.electronAPI.playlist.start('$plid', '$screen')" >/dev/null
    wait_assigned $B 40 && pass "daytime playlist: past the first item's end the next one is shown" || fail "daytime playlist (past end): $(assigned)"
fi
cdp "window.electronAPI.playlist.delete('$plid')" >/dev/null

# copy and move between library folders
sshv "rm -rf $LIBS; mkdir -p $LIB_A $LIB_B && cp -a ~/lwe/wp-$A $LIB_A/$LOCAL_A && cp -a ~/lwe/wp-$A $LIB_A/$LOCAL_B"
cdp "window.electronAPI.config.setExtraLibraries([{path: '$LIB_A', name: 'feat-a'}, {path: '$LIB_B', name: 'feat-b'}])" >/dev/null
cdp 'window.electronAPI.library.scan()' >/dev/null
[ "$(cdp "window.electronAPI.library.getOne('$LOCAL_A').then(w => w ? w.source : 'none')" | tr -d '"')" = local ] && pass "extra library folder: its wallpapers are imported as local" || fail "extra library folder not scanned"
out=$(cdp "window.electronAPI.library.transfer(['$LOCAL_A'], '$LIB_B', 'copy').then(r => r.done + ' ' + r.alreadyThere + ' ' + r.failed.length)" | tr -d '"')
[ "$out" = "1 0 0" ] && sshv "test -f $LIB_B/$LOCAL_A/project.json && test -f $LIB_A/$LOCAL_A/project.json" && pass "transfer: copy leaves the original and fills the target ($out)" || fail "transfer copy: $out"
out=$(cdp "window.electronAPI.library.transfer(['$LOCAL_A'], '$LIB_B', 'copy').then(r => r.done + ' ' + r.alreadyThere + ' ' + r.failed.length)" | tr -d '"')
[ "$out" = "1 0 0" ] && sshv "test -f $LIB_B/$LOCAL_A/project.json" && pass "transfer: an identical folder at the target is reused ($out)" || fail "transfer copy again: $out"
out=$(cdp "window.electronAPI.library.transfer(['$LOCAL_B'], '$LIB_B', 'move').then(r => r.done + ' ' + r.alreadyThere + ' ' + r.failed.length)" | tr -d '"')
if [ "$out" = "1 0 0" ] && sshv "test -f $LIB_B/$LOCAL_B/project.json && ! test -e $LIB_A/$LOCAL_B"; then pass "transfer: move removes the original ($out)"; else fail "transfer move: $out"; fi
out=$(cdp "window.electronAPI.library.transfer(['$LOCAL_B'], '$LIB_B', 'copy').then(r => r.done + ' ' + r.alreadyThere + ' ' + r.failed.length)" | tr -d '"')
[ "$out" = "0 1 0" ] && pass "transfer: a wallpaper already in that library is left alone ($out)" || fail "transfer into its own library: $out"

# custom thumbnails (engine screenshot of the wallpaper)
meta=$(cdp "window.electronAPI.wallpaper.generateThumbnail('$LOCAL_A').then(w => w.customPreview || 'none')" | tr -d '"')
if [ -n "$meta" ] && [ "$meta" != none ] && [[ $meta != *Error* ]] && sshv "test -s '$meta'"; then
    pass "thumbnail: captured to $(basename "$meta")"
    meta=$(cdp "window.electronAPI.wallpaper.deleteThumbnail('$LOCAL_A').then(w => w.customPreview || 'none')" | tr -d '"')
    [ "$meta" = none ] && pass "thumbnail: deleting it restores the original preview" || fail "thumbnail still set after delete: $meta"
else
    fail "thumbnail capture: $meta"
fi

# audio inputs and the graphics notice answer
out=$(cdp "window.electronAPI.lwe.listAudioInputs().then(a => Array.isArray(a) ? 'list ' + a.length : 'bad')" | tr -d '"')
[[ $out == list* ]] && pass "audio input devices are listed ($out)" || fail "audio input list: $out"
out=$(cdp "window.electronAPI.graphics.warning().then(w => w ? w.kind : 'none')" | tr -d '"')
case $out in
none|software|flatpak-nvidia) pass "graphics notice: $out" ;;
*) fail "graphics notice answered: $out" ;;
esac

skip "Verify files, Repair and backup of workshop items need a Steam client in the guest"
cleanup
exit $status
