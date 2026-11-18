#!/usr/bin/env python3
"""The v4 demo's TLS trust anchors (wget https://) from a PEM bundle of root CAs -- curl's
https://curl.se/ca/cacert.pem, Mozilla's roots: each root's subject name and public key, as BearSSL's
br_x509_trust_anchor wants them, in one blob the demo builds in (demo/tls_anchors.S .incbin's it), and a list
of the roots for people (demo/tls_anchors.txt).

    python tools/tls_anchors.py cacert.pem

The blob (little endian): "TA01", the count (u32), then per root: its key's type (u8: 1 RSA, 2 EC), the EC
curve (u8: BearSSL's / TLS's number, 23 P-256, 24 P-384, 25 P-521; 0 for RSA), the lengths (u16 each) of the
subject name's DER, of the RSA modulus or EC point, and of the RSA exponent (0 for EC), then those bytes.
Roots with other keys (none in Mozilla's set as of 2026) are left out and named.  The same bundle gives the
same bytes."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct
import sys

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

ROOT = Path(__file__).resolve().parents[1]
DEMO = ROOT / "firmware" / "maciej_app" / "demo"
CURVES = {"secp256r1": 23, "secp384r1": 24, "secp521r1": 25}


def der_item(data: bytes, at: int) -> tuple[int, int, int]:
    """The DER element at `at`: (its tag, where its contents start, where it ends)."""
    tag = data[at]
    n = data[at + 1]
    at += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(data[at:at + k], "big")
        at += k
    return tag, at, at + n


def raw_subject(cert: x509.Certificate) -> bytes:
    """The subject name exactly as the certificate encodes it (what BearSSL compares an issuer name with)."""
    tbs = cert.tbs_certificate_bytes
    _tag, at, end = der_item(tbs, 0)              # TBSCertificate
    items = []
    while at < end:
        tag, start, stop = der_item(tbs, at)
        items.append((tag, at, stop))
        at = stop
    if items[0][0] == 0xA0:                        # [0] version
        items = items[1:]
    _tag, start, stop = items[4]                   # serial, signature, issuer, validity, subject
    return tbs[start:stop]


def strip(n: int) -> bytes:
    return n.to_bytes(max(1, (n.bit_length() + 7) // 8), "big")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("bundle", type=Path, help="a PEM bundle (curl's cacert.pem)")
    parser.add_argument("--out", type=Path, default=DEMO / "tls_anchors.bin")
    parser.add_argument("--list", type=Path, default=DEMO / "tls_anchors.txt")
    args = parser.parse_args()
    text = args.bundle.read_bytes()
    dated = next((line.decode("ascii", "replace").strip("# \r") for line in text.splitlines()
                  if line.startswith(b"## Certificate data from Mozilla")), "")
    certs = x509.load_pem_x509_certificates(text)
    blob, names, left = bytearray(), [], []
    for cert in certs:
        key = cert.public_key()
        name = cert.subject.rfc4514_string()
        dn = raw_subject(cert)
        if isinstance(key, rsa.RSAPublicKey):
            numbers = key.public_numbers()
            kind, curve, a, b = 1, 0, strip(numbers.n), strip(numbers.e)
            what = f"RSA {key.key_size}"
        elif isinstance(key, ec.EllipticCurvePublicKey) and key.curve.name in CURVES:
            kind, curve = 2, CURVES[key.curve.name]
            a = key.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
            b = b""
            what = f"EC {key.curve.name}"
        else:
            left.append(name)
            continue
        blob += struct.pack("<BBHHH", kind, curve, len(dn), len(a), len(b)) + dn + a + b
        fingerprint = hashlib.sha256(cert.public_bytes(serialization.Encoding.DER)).hexdigest()
        names.append(f"{fingerprint}  {what:<15}  {cert.not_valid_after_utc:%Y-%m-%d}  {name}")
    out = b"TA01" + struct.pack("<I", len(names)) + bytes(blob)
    args.out.write_bytes(out)
    lines = [f"The roots the v4 demo's wget trusts for https:// ({len(names)}; tools/tls_anchors.py made "
             f"tls_anchors.bin from curl's cacert.pem, {dated or 'Mozilla data'}).",
             "SHA-256 of the certificate, its key, valid until, subject:", ""] + names
    if left:
        lines += ["", "Left out (a key BearSSL's minimal X.509 engine does not take):"] + left
    args.list.write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    print(f"{args.out}: {len(names)} roots, {len(out)} bytes" + (f"; {len(left)} left out" if left else ""))


if __name__ == "__main__":
    sys.exit(main())
