# usage: ours_pause.py <scene dir> <out dir>
# this engine through the same pause (mocked UPower OnBattery with --fps-battery 0 at 6 s, back at 12 s) as we_pause.sh,
# on :98 (ours_window.sh), frame numbers in <out dir>/frames.txt
import os, subprocess, sys, time, signal
R = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../..'))
scene = sys.argv[1]; out = sys.argv[2]
os.makedirs(out, exist_ok=True)
addr = out + '/sysbus.addr'
try: os.remove(addr)
except FileNotFoundError: pass
fs = subprocess.Popen([sys.executable, R + '/tools/desktops/fake_system.py', addr], stdout=open(out + '/fs.log', 'w'), stderr=subprocess.STDOUT)
for _ in range(100):
    if os.path.exists(addr) and open(addr).read().strip(): break
    time.sleep(0.1)
bus = open(addr).read().strip()
env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=bus)
eng = subprocess.Popen(['bash', R + '/tools/wecompare/wine/ours_window.sh', scene, '40', '--fps-battery', '0', '--fps', '30'], env=env,
                       stdout=open(out + '/engine.log', 'w'), stderr=subprocess.STDOUT)
time.sleep(float(os.environ.get('LOAD', 8)))
grab = subprocess.Popen([sys.executable, os.path.expanduser('~/.local/share/we_live/tests/videotex/grab_ours.py'), out + '/frames.txt', '24', ':98', '0', '0', '1920', '1080'])
import dbus
def setbat(v):
    b = dbus.bus.BusConnection(bus)
    o = dbus.Interface(b.get_object('org.freedesktop.UPower', '/org/freedesktop/UPower'), 'org.freedesktop.DBus.Mock')
    o.UpdateProperties('org.freedesktop.UPower', {'OnBattery': dbus.Boolean(v)})
t0 = time.time(); ev = []
time.sleep(6); setbat(True); ev.append((time.time() - t0, 'pause'))
time.sleep(6); setbat(False); ev.append((time.time() - t0, 'play'))
grab.wait()
open(out + '/events.txt', 'w').write(''.join('%.3f %s\n' % e for e in ev))
eng.terminate(); fs.terminate()
