#!/usr/bin/env python3
"""Build the TLS root-certificate bundle that yOS uses for HTTPS (GitHub updates).

Writes data/x509_crt_bundle.bin in the ESP-IDF certificate-bundle format: a big-endian
count, then per certificate (sorted by subject DER) the name length, key length,
subject DER and SubjectPublicKeyInfo DER.

  tools/gen_cert_bundle.py [roots.pem]    default: /etc/ssl/cert.pem (macOS system roots)
"""
import struct
import sys
import warnings
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import serialization

src = sys.argv[1] if len(sys.argv) > 1 else "/etc/ssl/cert.pem"
out = Path(__file__).resolve().parent.parent / "data" / "x509_crt_bundle.bin"
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
bundle = struct.pack(">H", len(entries))
for name in sorted(entries):
    key = entries[name]
    bundle += struct.pack(">HH", len(name), len(key)) + name + key
out.parent.mkdir(exist_ok=True)
out.write_bytes(bundle)
print(f"{len(entries)} roots, {len(bundle)} bytes -> {out}")
