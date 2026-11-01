#!/usr/bin/env python3
"""Extract the data partition of a Wii .rvz (Dolphin's compressed format) into a Dolphin-style folder (sys/, files/).
Usage: extract_rvz.py game.rvz outdir     Needs Python 3.14 (compression.zstd) or: pip install zstandard

RVZ keeps a partition's data already decrypted, with the hash blocks stripped, so nothing is decrypted here. The
layout is Dolphin's docs/WiaAndRvz.md. Junk (the padding a disc fills its gaps with, which RVZ stores as a seed) is
read back as zeros: files never contain it, and the extraction reads only files and the system area."""
import struct, sys
from extract_wbfs import extract

try:
    from compression.zstd import decompress as zstd_decompress
except ImportError:
    import zstandard
    def zstd_decompress(data):
        return zstandard.ZstdDecompressor().decompressobj().decompress(data)

SECTOR = 0x8000          # an encrypted Wii sector
PAYLOAD = 0x7C00         # its data without the hash block
u32 = lambda b, o=0: struct.unpack_from(">I", b, o)[0]
u64 = lambda b, o=0: struct.unpack_from(">Q", b, o)[0]


class Rvz:
    def __init__(self, path):
        self.f = open(path, "rb")
        h1 = self.f.read(0x48)
        assert h1[:4] in (b"RVZ\x01", b"WIA\x01"), "not an RVZ file"
        self.rvz = h1[:4] == b"RVZ\x01"
        h2 = self.f.read(u32(h1, 0x0C))
        self.compression, self.chunk = u32(h2, 0x04), u32(h2, 0x0C)
        assert self.compression in (0, 5), f"compression {self.compression} unsupported (only none and zstd)"
        self.disc_header = h2[0x10:0x90]
        n_part, part_size, part_off = u32(h2, 0x90), u32(h2, 0x94), u64(h2, 0x98)
        n_raw, raw_off, raw_size = u32(h2, 0xB4), u64(h2, 0xB8), u32(h2, 0xC0)
        n_grp, grp_off, grp_size = u32(h2, 0xC4), u64(h2, 0xC8), u32(h2, 0xD0)
        self.parts = []  # (first_sector, sectors, group_index, groups) for each data entry, both halves of each partition
        tbl = self._at(part_off, n_part * part_size)
        for i in range(n_part):
            e = tbl[i * part_size + 16:]
            self.parts.append([struct.unpack_from(">IIII", e, 16 * k) for k in range(2)])
        raw = self._table(raw_off, raw_size)
        self.raw = [struct.unpack_from(">QQII", raw, 0x18 * i) for i in range(n_raw)]
        grp = self._table(grp_off, grp_size)
        size = 0x0C if self.rvz else 0x08
        self.groups = [struct.unpack_from(">III" if self.rvz else ">II", grp, size * i) for i in range(n_grp)]
        self.cache = {}
        self.holes = 0

    def _at(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def _table(self, off, n):
        data = self._at(off, n)
        return zstd_decompress(data) if self.compression == 5 else data

    def group(self, index, partition):
        """Group `index`'s data: partition payload (hash exceptions dropped) or raw disc bytes."""
        if index in self.cache:
            return self.cache[index]
        entry = self.groups[index]
        offset, size = entry[0] << 2, entry[1]
        packed = entry[2] if self.rvz else 0
        compressed = bool(size & 0x80000000) if self.rvz else self.compression != 0
        size &= 0x7FFFFFFF
        if size == 0:
            data = b""  # all zeros; the reader pads
        else:
            data = self._at(offset, size)
            if compressed:
                data = zstd_decompress(data)
            if partition:
                # Hash exceptions: one list per 2 MiB of chunk, each a count and 22-byte entries.
                pos = 0
                for _ in range(max(1, self.chunk // 0x200000)):
                    pos += 2 + 22 * struct.unpack_from(">H", data, pos)[0]
                if not compressed:
                    pos = (pos + 3) & ~3
                data = data[pos:]
            if packed:
                out, pos = bytearray(), 0
                while pos < len(data):
                    n = u32(data, pos); pos += 4
                    if n & 0x80000000:
                        out += bytes(n & 0x7FFFFFFF); pos += 68  # junk: a seed in the file, zeros here
                    else:
                        out += data[pos:pos + n]; pos += n
                data = bytes(out)
        if len(self.cache) > 8:
            self.cache.clear()
        self.cache[index] = data
        return data

    def _span(self, data, off, n, total):
        """n bytes at off from group data that stands for `total` bytes (short data means trailing zeros)."""
        chunk = data[off:off + n]
        return chunk + bytes(min(n, total - off) - len(chunk))

    def read(self, off, n):
        """Raw (unencrypted part of the) disc: the header and partition table."""
        out = bytearray()
        while n > 0:
            if off < 0x80:
                c = min(n, 0x80 - off)
                out += self.disc_header[off:off + c]
            else:
                for data_off, data_size, gi, ng in self.raw:
                    start = data_off - data_off % SECTOR  # an entry's groups start at its sector
                    if start <= off < data_off + data_size:
                        break
                else:
                    raise IOError(f"offset {off:#x} is in no raw data entry")
                rel = off - start
                g, o = divmod(rel, self.chunk)
                c = min(n, self.chunk - o, data_off + data_size - off)
                out += self._span(self.group(gi + g, False), o, c, self.chunk)
            off += c; n -= c
        return bytes(out)


class RvzPartition:
    """A partition's decrypted data, as Partition.read in extract_wbfs.py gives it."""
    def __init__(self, disc, offset):
        self.disc = disc
        data_start = offset + (u32(disc.read(offset + 0x2B8, 4)) << 2)
        first = data_start // SECTOR
        self.entries = next(e for e in disc.parts if e[0][0] == first)
        self.first = first
        self.per_group = disc.chunk // SECTOR

    def read(self, off, n):
        out = bytearray()
        while n > 0:
            sector, o = divmod(off, PAYLOAD)
            sector += self.first
            for first, count, gi, ng in self.entries:
                if first <= sector < first + count:
                    break
            else:
                raise IOError(f"partition offset {off:#x} is in no data entry")
            g, s = divmod(sector - first, self.per_group)
            o += s * PAYLOAD
            c = min(n, PAYLOAD - off % PAYLOAD)
            out += self.disc._span(self.disc.group(gi + g, True), o, c, self.per_group * PAYLOAD)
            off += c; n -= c
        return bytes(out)


def main(src, dst):
    disc = Rvz(src)
    extract(disc, lambda off: RvzPartition(disc, off), dst)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
