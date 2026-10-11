#!/usr/bin/env python3
"""Packs the Internet Archive Channel WAD: content 0 is the banner (opening.bnr), content 1 the
forwarder DOL (the boot content), which loads sd:/apps/iachannel/boot.dol.

    tools/mkwad.py --banner opening.bnr --dol forwarder.dol --out iachannel.wad [--certs cert.bin]
    tools/mkwad.py ... --nand-dir <Dolphin user>/Wii   (also installs it, decrypted, the way Dolphin
                                                        keeps titles: for testing)

The TMD and ticket are written from scratch and fakesigned (installing needs a WAD manager that
accepts fakesigned titles, as with any homebrew channel). Encryption uses libWiiPy
(pip install libWiiPy), which also fetches the standard certificate chain from Nintendo's update
server when --certs isn't given; nothing key- or certificate-related is stored in this repo.

Title ID 00010001-UIAC ("U" isn't a prefix Nintendo used for retail channels or WiiWare).
"""
import argparse
import hashlib
import os
import struct
import sys

TITLE_ID = 0x0001000155494143  # 00010001-UIAC
IOS = 58                       # USB 2.0 support; what the Homebrew Channel loads apps with
TITLE_VERSION = 1
ACCESS_AHB = 1                 # hardware access like apps started from the Homebrew Channel

TMD_ISSUER = b"Root-CA00000001-CP00000004"
TIK_ISSUER = b"Root-CA00000001-XS00000003"


def raw_tmd():
    t = bytearray(0x1E4)
    struct.pack_into(">I", t, 0, 0x00010001)         # RSA-2048 signature (zeroed by fakesigning)
    t[0x140:0x140 + len(TMD_ISSUER)] = TMD_ISSUER
    struct.pack_into(">Q", t, 0x184, 0x0000000100000000 | IOS)
    struct.pack_into(">Q", t, 0x18C, TITLE_ID)
    struct.pack_into(">I", t, 0x194, 1)              # title type: Wii
    t[0x198:0x19A] = b"01"                           # group (publisher)
    struct.pack_into(">H", t, 0x19C, 3)              # region free
    struct.pack_into(">I", t, 0x1D8, ACCESS_AHB)
    struct.pack_into(">H", t, 0x1DC, TITLE_VERSION)
    struct.pack_into(">H", t, 0x1DE, 0)              # contents are added below
    struct.pack_into(">H", t, 0x1E0, 1)              # boot index: the forwarder
    return bytes(t)


def raw_ticket(title_key):
    from libWiiPy.title.crypto import encrypt_title_key
    t = bytearray(0x2A4)
    struct.pack_into(">I", t, 0, 0x00010001)
    t[0x140:0x140 + len(TIK_ISSUER)] = TIK_ISSUER
    t[0x1BF:0x1CF] = encrypt_title_key(title_key, 0, struct.pack(">Q", TITLE_ID))
    struct.pack_into(">Q", t, 0x1D0, TITLE_ID)       # ticket ID
    struct.pack_into(">Q", t, 0x1DC, TITLE_ID)
    struct.pack_into(">H", t, 0x1E4, 0xFFFF)
    struct.pack_into(">H", t, 0x1E6, TITLE_VERSION)
    t[0x222:0x262] = b"\xff" * 0x40                  # all contents accessible
    return bytes(t)


def cert_chain(path):
    if path and os.path.exists(path):
        return open(path, "rb").read()
    from libWiiPy.title.nus import download_cert_chain
    print("fetching the certificate chain from Nintendo's update server")
    data = download_cert_chain()
    if path:
        with open(path, "wb") as f:
            f.write(data)
    return data


def install_nand(nand, title, contents):
    """Installs into a Dolphin NAND folder (Dolphin keeps contents decrypted)."""
    hi, lo = "%08x" % (TITLE_ID >> 32), "%08x" % (TITLE_ID & 0xFFFFFFFF)
    cdir = os.path.join(nand, "title", hi, lo, "content")
    os.makedirs(cdir, exist_ok=True)
    os.makedirs(os.path.join(nand, "title", hi, lo, "data"), exist_ok=True)
    with open(os.path.join(cdir, "title.tmd"), "wb") as f:
        f.write(title.tmd.dump())
    for cid, data in contents:
        with open(os.path.join(cdir, "%08x.app" % cid), "wb") as f:
            f.write(data)
    tdir = os.path.join(nand, "ticket", hi)
    os.makedirs(tdir, exist_ok=True)
    with open(os.path.join(tdir, lo + ".tik"), "wb") as f:
        f.write(title.ticket.dump())
    print(f"installed into {nand}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--banner", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--certs", help="certificate chain file (cached here after the first download)")
    ap.add_argument("--nand-dir", help="also install into this Dolphin NAND folder (…/Wii)")
    a = ap.parse_args()
    try:
        from libWiiPy.title.title import Title
    except ImportError:
        sys.exit("mkwad.py needs libWiiPy: pip install libWiiPy")

    # A fixed title key keeps builds reproducible; it protects nothing (anyone can decrypt a WAD).
    title_key = hashlib.sha1(b"iachannel title key").digest()[:16]
    t = Title()
    t.load_cert_chain(cert_chain(a.certs))
    t.load_tmd(raw_tmd())
    t.load_ticket(raw_ticket(title_key))
    contents = [(0, open(a.banner, "rb").read()), (1, open(a.dol, "rb").read())]
    from libWiiPy.title.crypto import encrypt_content
    for cid, data in contents:  # content ID = index; type 1: normal content
        t.add_enc_content(encrypt_content(data, title_key, cid), cid, cid, 1, len(data),
                          hashlib.sha1(data).hexdigest().encode())
    t.fakesign()
    wad = t.dump_wad()
    with open(a.out, "wb") as f:
        f.write(wad)
    print(f"{a.out}: {len(wad)} bytes, title {TITLE_ID:016x}, IOS{IOS}")
    if a.nand_dir:
        install_nand(a.nand_dir, t, contents)


if __name__ == "__main__":
    main()
