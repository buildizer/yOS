#!/usr/bin/env python3
"""Send a firmware image to a running yOS over the USB cable (see src/services/usb_update.cpp).

usage: usb_update.py firmware.bin /dev/cu.usbserial-XXX [baud] [--raw]
Exit code 3 means yOS didn't answer (no firmware with USB update support running): use recovery.

Boards with protocol 2 take the whole image into memory at 1 Mbaud with 16 chunks in flight, then
write flash. Older boards get the original stop-and-wait transfer at 921600. If 1 Mbaud turns out to
be unreliable on this cable, the transfer is cancelled and restarted at 921600 automatically.
"""
import hashlib, struct, sys, time, zlib
import serial

CHUNK = 1024


class Retry(Exception):
    """The line is too noisy at this speed: start over slower."""


def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False  # keep the board's auto-reset lines released so opening the port doesn't reboot it
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def protocol(s):
    # Older firmware ignores YOSVER. Ask twice: a board that is still busy starting up answers late.
    for _ in range(2):
        s.write(b"\nYOSVER\n")
        t0 = time.time()
        while time.time() - t0 < 1.5:
            line = s.readline().decode(errors='replace').strip()
            if line.startswith('YOSPROTO '):
                return int(line.split()[1])
    return 1


def wait_listening(s):
    """After a cancelled fast transfer the board may have missed the cancel; it gives up on its own after a
    few idle seconds and returns to 115200. Wait until it answers again."""
    s.baudrate = 115200
    t0 = time.time()
    while time.time() - t0 < 20:
        s.reset_input_buffer()
        s.write(b"\nYOSVER\n")
        t1 = time.time()
        while time.time() - t1 < 1:
            if s.readline().decode(errors='replace').strip().startswith('YOSPROTO'):
                return
    sys.exit("yOS stopped answering. Unplug and replug the board, then try again.")


def transfer(s, raw, image, md5, baud, compressed, proto, fallback=False):
    s.baudrate = 115200
    s.reset_input_buffer()
    zfield = f" {len(image)}" if compressed else ''
    s.write(f"\nYOSUPD {len(raw)} {md5} {baud}{zfield}\n".encode())
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

    # Go-back-N. Protocol 2 boards only buffer while receiving, so many chunks can be in flight;
    # older boards write flash between chunks and only have room for two.
    window = 16 if proto >= 2 else 2
    total = (len(image) + CHUNK - 1) // CHUNK
    base, nxt, retries, start = 0, 0, 0, time.time()
    last_progress = time.time()

    def send(i):
        data = image[i * CHUNK:(i + 1) * CHUNK]
        s.write(b'C' + struct.pack('<IHI', i, len(data), zlib.crc32(data) & 0xFFFFFFFF) + data)

    while base < total:
        while nxt < total and nxt < base + window:
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
        # A noisy fast line shows up early; give up on this speed while it is cheap to restart.
        if proto >= 2 and baud > 921600 and fallback and retries > 20 and base < total // 4:
            s.write(b'ABRT')
            time.sleep(0.3)
            raise Retry()
        if retries > 300:
            sys.exit("\nToo many transfer errors. Check the cable, or use recovery (option 3).")
        pct = base * 100 // total
        print(f"\rUploading {pct:3d}%  ({base}/{total} chunks, {retries} retries)", end='', flush=True)
    sent_at = time.time()

    s.write(b'C' + struct.pack('<IHI', total, 0, 0))
    t0 = time.time()
    while time.time() - t0 < 120:  # protocol 2 writes all of flash now
        line = s.readline().decode(errors='replace').strip()
        if line == 'WRITING':
            print(f"\nSent in {sent_at - start:.1f} s. Writing to flash on the board…", flush=True)
        elif line.startswith('STATS '):
            print("  " + line[6:])
        elif line == 'DONE':
            print(f"Verified. yOS is restarting ({time.time() - start:.1f} s, {retries} retries).")
            return
        elif line.startswith('FAIL'):
            sys.exit(f"\nyOS rejected the image: {line}")
        elif line.startswith('ERR '):  # the end marker got mangled: send it again
            s.write(b'C' + struct.pack('<IHI', total, 0, 0))
    sys.exit("\nNo confirmation from yOS after the last chunk.")


def main():
    raw_mode = '--raw' in sys.argv  # for boards running firmware from before compressed transfers
    args = [a for a in sys.argv[1:] if a != '--raw']
    path, port = args[0], args[1]
    raw = open(path, 'rb').read()
    md5 = hashlib.md5(raw).hexdigest()
    image = raw if raw_mode else zlib.compress(raw, 9)  # what travels; yOS inflates it with the ROM decompressor

    s = open_port(port)
    proto = protocol(s)
    if len(args) > 2:
        speeds = [int(args[2])]
    else:
        # Measured on the Guition board's CH340: 1 Mbaud is clean, 1.5 and 2 Mbaud lose data.
        speeds = [1000000, 921600] if proto >= 2 else [921600]
    print(f"Sending {len(image) // 1024} KB (compressed from {len(raw) // 1024} KB), protocol {proto}")
    for i, baud in enumerate(speeds):
        try:
            print(f"Line speed {baud} baud")
            return transfer(s, raw, image, md5, baud, not raw_mode, proto, fallback=i + 1 < len(speeds))
        except Retry:
            print(f"\n{baud} baud is unreliable on this cable; trying {speeds[i + 1]}.")
            wait_listening(s)


if __name__ == '__main__':
    main()
