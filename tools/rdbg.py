#!/usr/bin/env python3
"""USB-serial remote for the Cardputer Roast firmware.
usage: rdbg.py <cmd> [<cmd> ...]
  key <name>        press a key (a..z 0..9 enter esc tab space bksp up down left right; S-<name> = with Aa)
  hold <key> <ms>   hold a key down for <ms> milliseconds
  type <text>       type characters
  shot <file>       save the screen (240x135) as PNG
  quiet             silence all beeps until the next reboot
  ping              print the device's PONG line (uptime, free heap)
  raw <line> <sec>  send a console line and print the output for <sec> seconds, e.g.
                    raw regs 2 | raw 'ls /sdcard/roast/logs' 2 | raw 'cat <file>' 3 | raw sdinfo 2
  wait <sec>        sleep
  log <sec>         print the device log for <sec> seconds
The port is the first /dev/cu.usbmodem* or /dev/ttyACM*; set RDBG_PORT to override.
"""
import glob, os, struct, sys, time, zlib
import serial

def open_port():
    ports = [os.environ['RDBG_PORT']] if os.environ.get('RDBG_PORT') else \
        sorted(glob.glob('/dev/cu.usbmodem*')) + sorted(glob.glob('/dev/ttyACM*'))
    if not ports:
        sys.exit('no /dev/cu.usbmodem* or /dev/ttyACM* port (set RDBG_PORT)')
    # Keep the OS default DTR/RTS. Forcing both low pulses the ESP32-S3 USB-Serial-JTAG reset, and a reset
    # under the community Launcher boots straight into the app (sometimes into a hung state).
    s = serial.Serial()
    s.port = ports[0]
    s.baudrate = 115200
    s.timeout = 0.2
    s.open()
    return s

def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

def shot(s, path):
    s.reset_input_buffer()
    s.write(b'shot\n')
    buf, deadline, w, h, rows, started = b'', time.time() + 8, 0, 0, [], False
    while time.time() < deadline:
        buf += s.read(4096)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            line = line.strip()
            if line.startswith(b'SHOT '):
                _, w, h = line.split()
                w, h, rows, started = int(w), int(h), [], True
            elif line == b'ENDSHOT' and started:
                out = []
                for r in rows[:h]:
                    px = bytes.fromhex(r.decode())
                    rgb = bytearray()
                    for i in range(0, len(px), 2):
                        v = (px[i] << 8) | px[i + 1]  # sprite stores RGB565 big-endian
                        rgb += bytes(((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
                    out.append(bytes(rgb))
                write_png(path, w, h, out)
                print('saved', path)
                return
            elif started and len(line) == w * 4:
                rows.append(line)
    print('shot timeout')

def main():
    s = open_port()
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        c = args[i]
        if c in ('key', 'type'):
            s.write(f'{c} {args[i + 1]}\n'.encode()); i += 2; time.sleep(0.25)
        elif c == 'shot':
            time.sleep(0.4); shot(s, args[i + 1]); i += 2
        elif c == 'wait':
            time.sleep(float(args[i + 1])); i += 2
        elif c == 'log':
            end = time.time() + float(args[i + 1])
            while time.time() < end:
                d = s.read(4096)
                if d: sys.stdout.write(d.decode(errors='replace'))
            i += 2
        elif c == 'raw':  # raw <line> <seconds>: send a command line and print device output for N seconds
            s.write((args[i + 1] + '\n').encode()); end = time.time() + float(args[i + 2])
            while time.time() < end:
                d = s.read(4096)
                if d: sys.stdout.write(d.decode(errors='replace')); sys.stdout.flush()
            i += 3
        elif c == 'hold':  # hold <key> <ms>
            s.write(f'hold {args[i + 1]} {args[i + 2]}\n'.encode()); time.sleep(int(args[i + 2]) / 1000 + 0.3); i += 3
        elif c == 'quiet':
            s.write(b'quiet\n'); i += 1; time.sleep(0.3)
        elif c == 'ping':
            s.reset_input_buffer(); s.write(b'ping\n'); time.sleep(0.5); print([x for x in s.read(65536).decode(errors='replace').splitlines() if 'PONG' in x]); i += 1
        else:
            sys.exit('unknown ' + c)

main()
