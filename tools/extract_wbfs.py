#!/usr/bin/env python3
"""Extract the data partition of a Wii .wbfs into a Dolphin-style folder (sys/, files/).
Usage: extract_wbfs.py game.wbfs outdir     Needs: pip install cryptography"""
import os, struct, sys

COMMON_KEY = bytes.fromhex("EBE42A225E8593E448D9C5457381AAF7")  # retail Wii common key (index 0)
u32 = lambda b, o=0: struct.unpack(">I", b[o:o + 4])[0]

def dec(key, iv, data):
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # only extraction decrypts; wbfs_convert.py needs just the Wbfs reader
    d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    return d.update(data) + d.finalize()

class Wbfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(512)
        assert h[:4] == b"WBFS", "not a WBFS file"
        hd, self.blk = 1 << h[8], 1 << h[9]
        n = (143432 * 2 * 0x8000) // self.blk  # table length covers a dual-layer disc
        self.f.seek(hd)
        self.wlba = struct.unpack(">%dH" % n, self.f.read(0x100 + n * 2)[0x100:])
        self.holes = 0  # reads that landed on blocks the image does not store

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            i, o = divmod(off, self.blk)
            c = min(n, self.blk - o)
            w = self.wlba[i] if i < len(self.wlba) else 0
            if w:
                self.f.seek(w * self.blk + o)
                out += self.f.read(c)
            else:
                self.holes += 1
                out += bytes(c)
            off += c; n -= c
        return bytes(out)

class Partition:
    def __init__(self, disc, off):
        self.disc = disc
        t = disc.read(off, 0x2C0)
        assert t[0x1F1] == 0, "only common key 0 is supported"
        self.key = dec(COMMON_KEY, t[0x1DC:0x1E4] + bytes(8), t[0x1BF:0x1CF])
        self.base = off + (u32(t, 0x2B8) << 2)
        self.ci, self.plain = -1, b""

    def read(self, off, n):  # off is in decrypted-data space
        out = bytearray()
        while n > 0:
            ci, o = divmod(off, 0x7C00)
            if ci != self.ci:
                raw = self.disc.read(self.base + ci * 0x8000, 0x8000)
                self.plain, self.ci = dec(self.key, raw[0x3D0:0x3E0], raw[0x400:]), ci
            c = min(n, 0x7C00 - o)
            out += self.plain[o:o + c]
            off += c; n -= c
        return bytes(out)

def main(src, dst):
    disc = Wbfs(src)
    extract(disc, lambda off: Partition(disc, off), dst)

# `disc` reads the raw disc (and counts `holes`); `open_partition(offset)` gives a reader of a partition's decrypted
# data. extract_rvz.py feeds its own readers through the same walk.
def extract(disc, open_partition, dst):
    gid = disc.read(0, 6)
    parts = []
    for g in range(4):
        cnt, tbl = struct.unpack(">II", disc.read(0x40000 + g * 8, 8))
        for i in range(cnt):
            off, typ = struct.unpack(">II", disc.read((tbl << 2) + i * 8, 8))
            parts.append((typ, off << 2))
    p = open_partition(next(off for typ, off in parts if typ == 0))
    boot = p.read(0, 0x440)
    assert boot[:6] == gid and boot[0x18:0x1C] == bytes.fromhex("5D1C9EA3"), "decryption check failed"
    dol_off, fst_off, fst_sz = (u32(boot, o) << 2 for o in (0x420, 0x424, 0x428))

    def put(rel, data_or_range):
        path = os.path.join(dst, *rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            if isinstance(data_or_range, bytes):
                f.write(data_or_range); return len(data_or_range)
            off, size = data_or_range
            for o in range(0, size, 0x100000):
                f.write(p.read(off + o, min(0x100000, size - o)))
            return size

    hdr = p.read(dol_off, 0x100)
    dol_sz = max(u32(hdr, 4 * i) + u32(hdr, 0x90 + 4 * i) for i in range(18) if u32(hdr, 0x90 + 4 * i))
    apl = p.read(0x2440, 0x20)
    put(["sys", "boot.bin"], boot)
    put(["sys", "bi2.bin"], p.read(0x440, 0x2000))
    put(["sys", "apploader.img"], p.read(0x2440, 0x20 + u32(apl, 0x14) + u32(apl, 0x18)))
    put(["sys", "main.dol"], (dol_off, dol_sz))
    put(["sys", "fst.bin"], p.read(fst_off, fst_sz))

    fst = p.read(fst_off, fst_sz)
    n = u32(fst, 8)
    names = fst[n * 12:]
    def name(i):
        o = u32(b"\0" + fst[i * 12 + 1:i * 12 + 4])
        return names[o:names.index(b"\0", o)].decode("utf-8", "replace")
    stats = {"files": 0, "bytes": 0, "holey": []}
    def walk(i, end, path):
        while i < end:
            a, b = u32(fst, i * 12 + 4), u32(fst, i * 12 + 8)
            if fst[i * 12]:
                walk(i + 1, b, path + [name(i)]); i = b
            else:
                h = disc.holes
                stats["bytes"] += put(["files"] + path + [name(i)], (a << 2, b))
                stats["files"] += 1
                if disc.holes != h: stats["holey"].append("/".join(path + [name(i)]))
                i += 1
    walk(1, n, [])
    print("game", gid.decode(), "| files", stats["files"], "| bytes", stats["bytes"], "| main.dol", dol_sz)
    print("files touching blocks missing from the image:", len(stats["holey"]))
    for x in stats["holey"][:30]: print("  MISSING", x)

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
