#!/usr/bin/env python3
"""Install an app image through the Launcher serial console (bmorcelli/Launcher 3.x):
   flash firmware <name> <size>  ->  READY <size>  ->  2048-byte chunks, each answered with ACK n/size  ->  OK flashed
usage: launcher_flash.py <name> <file.bin> | launcher_flash.py --cmd "<console command>" [seconds]"""
import glob, os, sys, time, serial

def catch(s, timeout=3):
    """After a power-on (or Esc from Roast) the Launcher shows its boot screen for ~1-5 s and then boots the
    installed app; answer the first 'Press the button' line with one 'nav sel' so it stays in the Launcher menu."""
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        buf += s.read(4096)
        if b'Press the button to enter the Launcher' in buf:
            s.write(b'nav sel\n'); time.sleep(1.5); s.reset_input_buffer(); print('caught Launcher boot screen'); return True
    print('no Launcher boot screen seen (already in menu?)'); return False

def open_port():
    p = [os.environ['RDBG_PORT']] if os.environ.get('RDBG_PORT') else \
        sorted(glob.glob('/dev/cu.usbmodem*')) + sorted(glob.glob('/dev/ttyACM*'))
    if not p: sys.exit('no /dev/cu.usbmodem* or /dev/ttyACM* port (set RDBG_PORT)')
    s = serial.Serial(); s.port = p[0]; s.baudrate = 115200; s.timeout = 0.2; s.open()
    return s

def read_until(s, pred, timeout):
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        buf += s.read(4096)
        lines = buf.decode(errors='replace').splitlines()
        for l in lines:
            if pred(l): return l, buf
    return None, buf

def cmd(s, line, wait):
    s.reset_input_buffer(); s.write((line + '\n').encode()); time.sleep(wait)
    print(s.read(65536).decode(errors='replace'))

s = open_port()
catch(s)
if sys.argv[1] == '--cmd':
    cmd(s, sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 2); sys.exit(0)
name, path = sys.argv[1], sys.argv[2]
data = open(path, 'rb').read()
s.reset_input_buffer()
s.write(f'flash firmware {name} {len(data)}\n'.encode())
l, raw = read_until(s, lambda x: x.startswith('READY') or x.startswith('ERR'), 30)
print('<<', l)
if not l or not l.startswith('READY'): print(raw.decode(errors='replace')[-800:]); sys.exit(1)
sent, t0 = 0, time.time()
while sent < len(data):
    chunk = data[sent:sent + 2048]; s.write(chunk); sent += len(chunk)
    l, raw = read_until(s, lambda x: x.startswith('ACK') or x.startswith('ERR'), 15)
    if not l or l.startswith('ERR'): print('!!', l, raw.decode(errors='replace')[-400:]); sys.exit(1)
    if (sent // 2048) % 100 == 0: print('ACK', sent, '/', len(data), round(time.time() - t0, 1), 's')
l, raw = read_until(s, lambda x: x.startswith('OK') or x.startswith('ERR'), 60)
print('<<', l, round(time.time() - t0, 1), 's')
sys.exit(0 if l and l.startswith('OK') else 1)
