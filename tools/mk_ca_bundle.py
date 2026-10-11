#!/usr/bin/env python3
"""Extract a small set of trusted roots from Mozilla's bundle (curl.se/ca/cacert.pem).

archive.org, web.archive.org, iiif.archive.org and the iaNNN.us.archive.org storage hosts
all chain to Go Daddy Root G2 (checked 2026-10). The others are spares in case IA changes CA.
A full bundle can be dropped on SD as sd:/apps/iachannel/cacert.pem to override this one.

Usage: tools/mk_ca_bundle.py deps/dl/cacert.pem assets/cacert.pem
"""
import sys

ROOTS = [
    "Go Daddy Root Certificate Authority - G2",
    "Starfield Root Certificate Authority - G2",
    "ISRG Root X1",
    "ISRG Root X2",
    "DigiCert Global Root G2",
    "GlobalSign Root CA - R3",
    "USERTrust RSA Certification Authority",
]


def main(src, dst):
    lines = open(src, encoding="utf-8").read().splitlines()
    found = {}
    for i, line in enumerate(lines):
        if line in ROOTS and i + 1 < len(lines) and lines[i + 1].startswith("="):
            start = lines.index("-----BEGIN CERTIFICATE-----", i)
            end = lines.index("-----END CERTIFICATE-----", start)
            found[line] = lines[start:end + 1]
    missing = [r for r in ROOTS if r not in found]
    if missing:
        sys.exit("missing roots: " + ", ".join(missing))
    with open(dst, "w", encoding="ascii", newline="\n") as out:
        for name in ROOTS:
            out.write(f"# {name}\n" + "\n".join(found[name]) + "\n")


if __name__ == "__main__":
    main(*sys.argv[1:3])
