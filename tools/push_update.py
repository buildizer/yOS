#!/usr/bin/env python3
"""Push a firmware image to yOS over the local network (see /api/update in src/services/web.cpp).

usage: push_update.py <host> [firmware.bin.gz | firmware.bin]
The default image is .pio/build/yos/firmware.bin.gz. The board's Update Password (Settings > General >
About) comes from YOS_OTA_PASS. yOS writes the image while it arrives, then checks it and restarts.
Exit code 2 means the board refused the password, 3 that it couldn't be reached or doesn't know
/api/update (older yOS: use espota once; tools/flash.sh ota falls back to it on its own).
"""
import base64, http.client, os, sys, time


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    host = sys.argv[1]
    path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), '..', '.pio', 'build', 'yos', 'firmware.bin.gz')
    password = os.environ.get('YOS_OTA_PASS', '')
    if not password:
        sys.exit("Set YOS_OTA_PASS to the board's Update Password.")
    image = open(path, 'rb').read()
    auth = base64.b64encode(f"yos:{password}".encode()).decode()
    print(f"Sending {os.path.basename(path)} ({len(image) // 1024} KB) to {host}")

    start = time.time()
    try:
        conn = http.client.HTTPConnection(host, 80, timeout=60)
        conn.putrequest('POST', '/api/update')
        conn.putheader('Authorization', f'Basic {auth}')
        conn.putheader('Content-Type', 'application/octet-stream')
        conn.putheader('Content-Length', str(len(image)))
        conn.endheaders()
        step = 16384
        for at in range(0, len(image), step):
            conn.send(image[at:at + step])
            pct = min(at + step, len(image)) * 100 // len(image)
            print(f"\rUploading {pct:3d}%", end='', flush=True)
        sent = time.time()
        print(f"\nSent in {sent - start:.1f} s. Waiting for yOS to finish writing…", flush=True)
        reply = conn.getresponse()
        body = reply.read().decode(errors='replace').strip()
    except (OSError, http.client.HTTPException) as e:
        print(f"\nCouldn't reach yOS at {host}: {e}")
        sys.exit(3)
    if reply.status == 401:
        print("yOS refused the Update Password.")
        sys.exit(2)
    if reply.status == 404:
        print("This yOS doesn't take pushed updates yet. Use espota once.")
        sys.exit(3)
    if reply.status != 200:
        sys.exit(f"yOS rejected the update: {body}")
    print(f"{body} (sent {sent - start:.1f} s, finished {time.time() - sent:.1f} s later, total {time.time() - start:.1f} s)")


if __name__ == '__main__':
    main()
