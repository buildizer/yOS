# PlatformIO post-build step (extra_scripts in platformio.ini).
#  1. Names the image: the app description every ESP-IDF image carries says "arduino-lib-builder" for
#     any Arduino sketch. It is patched in the ELF, before esptool turns that into firmware.bin (so the
#     image checksum and SHA-256 are computed over the patched bytes), to project "yOS" and the version
#     from include/version.h. yOS refuses update images that don't carry this name (firmware_update.cpp).
#  2. Writes firmware.bin.gz next to firmware.bin (gzip -9, no name or time stamp, so the same build
#     gives the same file). Updates travel compressed: about a third smaller.
import gzip, os, re, struct

Import("env")

APP_DESC_MAGIC = struct.pack("<I", 0xABCD5432)
PROJECT = b"yOS"


def firmware_version():
    text = open(os.path.join(env.subst("$PROJECT_DIR"), "include", "version.h")).read()
    m = re.search(r'#define\s+YOS_FIRMWARE_VERSION\s+"([^"]+)"', text)
    if not m:
        raise SystemExit("firmware_post.py: YOS_FIRMWARE_VERSION not found in include/version.h")
    return m.group(1).encode()


def name_image(target, source, env):
    path = str(target[0])
    elf = bytearray(open(path, "rb").read())
    # esp_app_desc_t: magic, secure_version, reserved[2], version[32] at 16, project_name[32] at 48
    hits = []
    at = elf.find(APP_DESC_MAGIC)
    while at >= 0:
        name = bytes(elf[at + 48:at + 80]).split(b"\0")[0]
        if name in (b"arduino-lib-builder", PROJECT) and elf[at + 16:at + 24] != b"\0" * 8:
            hits.append(at)
        at = elf.find(APP_DESC_MAGIC, at + 4)
    if len(hits) != 1:
        raise SystemExit("firmware_post.py: expected one app description in the ELF, found %d" % len(hits))
    at = hits[0]
    version = firmware_version()
    elf[at + 16:at + 48] = version[:31].ljust(32, b"\0")
    elf[at + 48:at + 80] = PROJECT.ljust(32, b"\0")
    open(path, "wb").write(elf)
    print("Image named %s %s" % (PROJECT.decode(), version.decode()))


def compress_image(target, source, env):
    path = str(target[0])
    raw = open(path, "rb").read()
    with open(path + ".gz", "wb") as f:
        with gzip.GzipFile(filename="", mode="wb", fileobj=f, compresslevel=9, mtime=0) as gz:
            gz.write(raw)
    print("firmware.bin %d bytes, firmware.bin.gz %d bytes (%.0f%%)"
          % (len(raw), os.path.getsize(path + ".gz"), 100.0 * os.path.getsize(path + ".gz") / len(raw)))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", name_image)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", compress_image)
