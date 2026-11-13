#!/usr/bin/env python3
"""Pull a roast recording from the Cardputer over USB and print its levels, second by second.
usage: pull_audio.py <log id> [seconds] [--out file.wav]
  <log id>   e.g. R0012: reads /sdcard/roast/logs/R0012.wav (and R0012.json for the event times)
  seconds    how much to pull from the start (default 60; 0 = the whole file, about 64 KB per second of audio)
The firmware's debug console serves the file in base64 chunks (`b64 <file> <offset> <length>`), so this is slow:
roughly 1 s of audio per 1-2 s. For whole roasts, take the SD card out instead.
Prints, per second: peak and RMS in dBFS, and how much of it is exact zeros (the silence the recorder inserts while a
tone plays or after an SD stall). Uses the same port rules as rdbg.py (RDBG_PORT to override).
"""
import argparse, base64, glob, json, math, os, re, struct, sys, time
import serial


def open_port():
    # Same rules as rdbg.py, kept local (importing rdbg would run its command line). Keep the OS default DTR/RTS:
    # forcing both low resets the ESP32-S3, and under the Launcher a reset boots straight into the app.
    ports = [os.environ['RDBG_PORT']] if os.environ.get('RDBG_PORT') else \
        sorted(glob.glob('/dev/cu.usbmodem*')) + sorted(glob.glob('/dev/ttyACM*'))
    if not ports:
        sys.exit('no /dev/cu.usbmodem* or /dev/ttyACM* port (set RDBG_PORT)')
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = ports[0], 115200, 0.2
    s.open()
    return s

CHUNK = 3072


def ask(s, line, prefix, timeout=5.0):
    s.write((line + '\n').encode())
    buf, deadline = b'', time.time() + timeout
    while time.time() < deadline:
        buf += s.read(8192)
        while b'\n' in buf:
            row, buf = buf.split(b'\n', 1)
            row = row.strip()
            # device log lines can run into the reply without a newline: match anywhere in the line
            for key in (prefix.encode() + b' ', b'ERR'):
                i = row.find(key)
                if i >= 0:
                    return row[i:].decode(errors='replace')
    raise TimeoutError(line)


def cat(s, path):
    s.reset_input_buffer()
    s.write(f'cat {path}\n'.encode())
    buf, deadline = b'', time.time() + 5
    while time.time() < deadline:
        buf += s.read(8192)
        if b'CATEND' in buf:
            break
    m = re.search(rb'CATBEGIN\r?\n(.*)\r?\nCATEND', buf, re.S)
    return m.group(1).decode(errors='replace') if m else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('id')
    ap.add_argument('seconds', nargs='?', type=float, default=60)
    ap.add_argument('--out')
    a = ap.parse_args()
    path = f'/sdcard/roast/logs/{a.id}.wav'
    s = open_port()
    s.reset_input_buffer()
    print(ask(s, 'rec', 'REC'))
    try:
        info = json.loads(cat(s, f'/sdcard/roast/logs/{a.id}.json')).get('audio', {})
    except ValueError:
        info = {}
    if info:
        print('json audio:', json.dumps({k: v for k, v in info.items() if k != 'silence'}))
        for g in info.get('silence', []):
            print(f"  silence at {g['at_ms'] / 1000:.2f} s for {g['ms']} ms ({g['why']})")
    rate = info.get('sample_rate', 32000)
    want = None if a.seconds == 0 else 512 + int(a.seconds * rate) * 2
    data, off, t0 = bytearray(), 0, time.time()
    while want is None or off < want:
        n = CHUNK if want is None else min(CHUNK, want - off)
        row = ask(s, f'b64 {path} {off} {n}', 'B64')
        if row.startswith('ERR'):
            sys.exit(row)
        parts = row.split(' ', 2)
        got = int(parts[1])
        if got == 0:
            break
        data += base64.b64decode(parts[2]) if len(parts) > 2 else b''
        off += got
        if off % (CHUNK * 64) == 0:
            print(f'  {off / 1024:.0f} KB, {off / 1024 / (time.time() - t0):.0f} KB/s', file=sys.stderr)
        if got < n:
            break
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        sys.exit('not a WAV file')
    ch, sr, bits = struct.unpack('<HIxxxxxxH', data[22:36])
    hdr_samples = struct.unpack('<I', data[508:512])[0] // 2
    pcm = data[512:]
    pcm = pcm[:len(pcm) // 2 * 2]
    samples = struct.unpack(f'<{len(pcm) // 2}h', pcm)
    print(f'{a.id}.wav: {ch} ch, {sr} Hz, {bits} bit; header says {hdr_samples / sr:.1f} s; pulled {len(samples) / sr:.1f} s')
    events = {v: k for k, v in info.get('events_ms', {}).items()}
    print(' sec   peak dBFS   rms dBFS   zeros')
    for sec in range(0, (len(samples) + sr - 1) // sr):
        w = samples[sec * sr:(sec + 1) * sr]
        pk = max(abs(x) for x in w) or 1
        rms = math.sqrt(sum(x * x for x in w) / len(w)) or 1
        zeros = sum(1 for x in w if x == 0) / len(w)
        mark = ' '.join(k for ms, k in events.items() if sec * 1000 <= ms < (sec + 1) * 1000)
        print(f'{sec:4d}   {20 * math.log10(pk / 32768):8.1f}   {20 * math.log10(rms / 32768):8.1f}   {zeros:5.0%}  {mark}')
    if a.out:
        out = bytearray(data[:512]) + pcm
        struct.pack_into('<I', out, 4, len(out) - 8)
        struct.pack_into('<I', out, 508, len(pcm))
        open(a.out, 'wb').write(out)
        print('wrote', a.out)


if __name__ == '__main__':
    main()
