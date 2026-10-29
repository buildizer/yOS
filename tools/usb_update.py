#!/usr/bin/env python3
"""Send a firmware image to a running yOS over the USB cable (see src/services/usb_update.cpp).

usage: usb_update.py firmware.bin /dev/cu.usbserial-XXX [baud] [--raw]
Exit code 3 means yOS didn't answer (no firmware with USB update support running): use recovery.
"""
import hashlib, struct, sys, time, zlib
import serial

CHUNK = 1024

def main():
    raw_mode = '--raw' in sys.argv  # for boards running firmware from before compressed transfers
    args = [a for a in sys.argv[1:] if a != '--raw']
    path, port = args[0], args[1]
    baud = int(args[2]) if len(args) > 2 else 921600
    raw = open(path, 'rb').read()
    md5 = hashlib.md5(raw).hexdigest()
    image = raw if raw_mode else zlib.compress(raw, 9)  # what travels; yOS inflates it with the ROM decompressor

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False  # keep the board's auto-reset lines released so opening the port doesn't reboot it
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    zfield = '' if raw_mode else f" {len(image)}"
    s.write(f"\nYOSUPD {len(raw)} {md5} {baud}{zfield}\n".encode())
    print(f"Sending {len(image) // 1024} KB (compressed from {len(raw) // 1024} KB)")

    t0 = time.time()
    while True:
        line = s.readline().decode(errors='replace').strip()
        if line == 'READY':
            break
        if line.startswith('FAIL'):
            sys.exit(f"yOS refused the update: {line}")
        if time.time() - t0 > 6:
            print("yOS didn't answer. Is it running a firmware with USB updates? Use recovery (option 3).")
            sys.exit(3)
    s.baudrate = baud
    time.sleep(0.15)
    s.reset_input_buffer()

    # Go-back-N with a small window: the next chunks travel while earlier ones are acknowledged.
    # Two 1 KB chunks in flight fit the board's 4 KB receive buffer even while it erases flash.
    total = (len(image) + CHUNK - 1) // CHUNK
    WINDOW = 2
    base, nxt, retries, start = 0, 0, 0, time.time()
    last_progress = time.time()
    def send(i):
        data = image[i * CHUNK:(i + 1) * CHUNK]
        s.write(b'C' + struct.pack('<IHI', i, len(data), zlib.crc32(data) & 0xFFFFFFFF) + data)
    while base < total:
        while nxt < total and nxt < base + WINDOW:
            send(nxt)
            nxt += 1
        line = s.readline().decode(errors='replace').strip()
        if line.startswith('OK '):
            acked = int(line[3:])
            if acked >= base:
                base = acked + 1
                last_progress = time.time()
        elif line.startswith('ERR '):
            want = int(line[4:])
            if want >= base:
                base = nxt = want  # resend from what the board expects
                retries += 1
                s.reset_input_buffer()
        elif line.startswith('FAIL'):
            sys.exit(f"\nyOS stopped the update: {line}")
        if time.time() - last_progress > 0.5:  # nothing acknowledged for a while: resend the window
            nxt = base
            retries += 1
            last_progress = time.time()
        if retries > 300:
            sys.exit("\nToo many transfer errors. Check the cable, or use recovery (option 3).")
        pct = base * 100 // total
        print(f"\rUploading {pct:3d}%  ({base}/{total} chunks, {retries} retries)", end='', flush=True)
    seq = total

    s.write(b'C' + struct.pack('<IHI', seq, 0, 0))
    t0 = time.time()
    while time.time() - t0 < 15:
        line = s.readline().decode(errors='replace').strip()
        if line == 'DONE':
            print(f"\nVerified. yOS is restarting ({time.time() - start:.0f} s, {retries} retries).")
            return
        if line.startswith('FAIL'):
            sys.exit(f"\nyOS rejected the image: {line}")
    sys.exit("\nNo confirmation from yOS after the last chunk.")

if __name__ == '__main__':
    main()
