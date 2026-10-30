#!/usr/bin/env python3
"""Build the TLS root-certificate bundle that yOS uses for HTTPS (GitHub updates).

Writes data/x509_crt_bundle.bin in the ESP-IDF 5 certificate-bundle format: one
little-endian uint32 offset per certificate (from the start of the bundle), then per
certificate (sorted by subject DER) the name length and key length (little-endian
uint16), subject DER and SubjectPublicKeyInfo DER. (ESP-IDF 4 used a big-endian count
and big-endian lengths; --convert rewrites such a bundle without the source PEM.)

  tools/gen_cert_bundle.py [roots.pem]    default: /etc/ssl/cert.pem (macOS system roots)
  tools/gen_cert_bundle.py --convert      data/x509_crt_bundle.bin from the ESP-IDF 4 format
"""
import struct
import sys
import warnings
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import serialization

out = Path(__file__).resolve().parent.parent / "data" / "x509_crt_bundle.bin"


def write(entries):
    """entries: {subject DER: SubjectPublicKeyInfo DER}"""
    names = sorted(entries)
    offsets, body = [], b""
    for name in names:
        offsets.append(4 * len(names) + len(body))
        body += struct.pack("<HH", len(name), len(entries[name])) + name + entries[name]
    bundle = struct.pack("<%dL" % len(offsets), *offsets) + body
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(bundle)
    print(f"{len(names)} roots, {len(bundle)} bytes -> {out}")


if sys.argv[1:] == ["--convert"]:
    old = out.read_bytes()
    count, at, entries = struct.unpack(">H", old[:2])[0], 2, {}
    for _ in range(count):
        name_len, key_len = struct.unpack(">HH", old[at:at + 4])
        name = old[at + 4:at + 4 + name_len]
        entries[name] = old[at + 4 + name_len:at + 4 + name_len + key_len]
        at += 4 + name_len + key_len
    if at != len(old):
        raise SystemExit("not an ESP-IDF 4 bundle")
    write(entries)
    sys.exit(0)

src = sys.argv[1] if len(sys.argv) > 1 else "/etc/ssl/cert.pem"
with warnings.catch_warnings():
    warnings.simplefilter("ignore")  # a few old roots have non-positive serial numbers; harmless here
    certs = x509.load_pem_x509_certificates(open(src, "rb").read())
entries = {}
for c in certs:
    name = c.subject.public_bytes()
    key = c.public_key().public_bytes(serialization.Encoding.DER,
                                      serialization.PublicFormat.SubjectPublicKeyInfo)
    if name in entries and entries[name] != key:
        # The device looks roots up by issuer name and can hold only one key per name.
        print(f"warning: two roots named {c.subject.rfc4514_string()}; keeping the first", file=sys.stderr)
        continue
    entries[name] = key
write(entries)
