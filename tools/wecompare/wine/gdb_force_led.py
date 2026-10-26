# Switches the LED plugin of a running WE 2.8.42 (wallpaper64.exe under Wine) on, as if a device were connected and the wallpaper's LEDs enabled:
#     LED_RAW=1 gdb -q -batch -p <wallpaper64.exe pid> -x gdb_force_led.py
# The plugin's options never arrive without the Wallpaper Engine UI, so the first controller's "wanted" and "granted"
# flags are set by hand (ledextensions64.dll +9 and +0xA). LED_RAW=1 also turns "Boost LED colors" off for raw frames
import gdb, os
gdb.execute('handle SIGSEGV SIGUSR1 SIGUSR2 SIGILL SIGTRAP SIGPIPE nostop noprint pass')
inf = gdb.selected_inferior()
def rd(a, n): return bytes(inf.read_memory(a, n))
def q(a): return int.from_bytes(rd(a, 8), 'little')
node = q(q(0x1404E6270))
plugin = q(node + 72)
wallpaper = q(q(plugin + 16))
controller = q(q(wallpaper + 48))
inf.write_memory(controller + 9, b'\x01')
inf.write_memory(controller + 10, b'\x01')
if os.environ.get('LED_RAW') == '1':
    inf.write_memory(wallpaper + 74, b'\x00')
print('forced', rd(controller + 8, 4).hex(), 'boost', rd(wallpaper + 74, 1).hex())
