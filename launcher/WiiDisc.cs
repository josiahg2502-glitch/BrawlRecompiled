using System.Buffers.Binary;
using System.Security.Cryptography;

namespace BrawlRecompiled;

// A Wii disc image (.iso, .ciso, .wbfs or .rvz): decodes the container, decrypts the data partition and extracts it
// to the Dolphin layout the game reads (sys/ and files/). Port of tools/extract_wbfs.py.
sealed class WiiDisc : IDisposable
{
    // Retail Wii common key (index 0), as in tools/extract_wbfs.py.
    static readonly byte[] CommonKey = Convert.FromHexString("EBE42A225E8593E448D9C5457381AAF7");
    const int Cluster = 0x8000, ClusterData = 0x7C00;

    readonly Container disc;
    readonly byte[] titleKey;
    readonly long dataBase;
    long cachedCluster = -1;
    byte[] cachedPlain = Array.Empty<byte>();

    public string GameId { get; }
    public int Revision { get; }
    public string Title { get; }
    public int Holes => disc.Holes;

    public static WiiDisc Open(string path) => new(path);

    WiiDisc(string path)
    {
        disc = Container.Open(path);
        var header = disc.Read(0, 0x60);
        GameId = System.Text.Encoding.ASCII.GetString(header, 0, 6);
        Revision = header[7];
        Title = System.Text.Encoding.ASCII.GetString(header, 0x20, 0x40).TrimEnd('\0', ' ');
        if (!GameId.All(c => c is >= '0' and <= '9' or >= 'A' and <= 'Z'))
            throw new InvalidDataException("This file is not a Wii disc image.");

        // Partition table: four groups of (count, table offset >> 2); type 0 is the game data.
        long dataPartition = -1;
        for (int g = 0; g < 4 && dataPartition < 0; g++)
        {
            var group = disc.Read(0x40000 + g * 8, 8);
            uint count = U32(group, 0), table = U32(group, 4);
            for (uint i = 0; i < count && i < 64; i++)
            {
                var entry = disc.Read(((long)table << 2) + i * 8, 8);
                if (U32(entry, 4) == 0) { dataPartition = (long)U32(entry, 0) << 2; break; }
            }
        }
        if (dataPartition < 0) throw new InvalidDataException("The image has no game data partition.");

        var ticket = disc.Read(dataPartition, 0x2C0);
        if (ticket[0x1F1] != 0) throw new InvalidDataException("Only discs using common key 0 are supported.");
        var iv = new byte[16];
        Array.Copy(ticket, 0x1DC, iv, 0, 8);
        titleKey = DecryptCbc(CommonKey, iv, ticket.AsSpan(0x1BF, 16));
        dataBase = dataPartition + ((long)U32(ticket, 0x2B8) << 2);

        var boot = ReadData(0, 0x20);
        if (System.Text.Encoding.ASCII.GetString(boot, 0, 6) != GameId || U32(boot, 0x18) != 0x5D1C9EA3u)
            throw new InvalidDataException("The partition did not decrypt correctly; the image is damaged or not a plain Wii disc.");
    }

    // The one disc the translation was made from: Super Smash Bros. Brawl, USA, revision 1 (the common print).
    public const string Game = "RSBE01";
    public const int GameRevision = 1;

    // "" when this is the disc the game runs, else why not.
    public string Problem()
    {
        if (GameId != Game)
            return $"That is {GameId} ({Title}), not Super Smash Bros. Brawl for the USA (RSBE01). Only that disc works.";
        if (Revision != GameRevision)
            return $"That is Super Smash Bros. Brawl USA revision {Revision}; only revision {GameRevision} works.";
        return "";
    }

    // Writes sys/ and files/ under outDir. Returns the number of files written.
    public int Extract(string outDir, Action<int, int, string>? progress = null, CancellationToken cancel = default)
    {
        var boot = ReadData(0, 0x440);
        long dolOff = (long)U32(boot, 0x420) << 2, fstOff = (long)U32(boot, 0x424) << 2, fstSize = (long)U32(boot, 0x428) << 2;
        var dolHeader = ReadData(dolOff, 0x100);
        long dolSize = 0;
        for (int i = 0; i < 18; i++)
            if (U32(dolHeader, 0x90 + 4 * i) != 0) dolSize = Math.Max(dolSize, (long)U32(dolHeader, 4 * i) + U32(dolHeader, 0x90 + 4 * i));
        var apl = ReadData(0x2440, 0x20);
        long aplSize = 0x20 + (long)U32(apl, 0x14) + U32(apl, 0x18);
        // The file table comes from the image: bounded, and every name a plain file name (#17), so a damaged or
        // crafted image can't write outside outDir or run away with memory.
        if (fstSize < 12 || fstSize > 16 << 20) throw new InvalidDataException("the disc's file table is damaged");
        var fst = ReadData(fstOff, (int)fstSize);
        int entries = (int)U32(fst, 8);
        if (entries < 1 || (long)entries * 12 > fst.Length) throw new InvalidDataException("the disc's file table is damaged");
        int nameBase = entries * 12;

        var files = new List<(string path, long off, long size)>();
        void Walk(int i, int end, string dir, int depth)
        {
            if (depth > 32) throw new InvalidDataException("the disc's file table is damaged");
            while (i < end)
            {
                int nameOff = (fst[i * 12 + 1] << 16) | (fst[i * 12 + 2] << 8) | fst[i * 12 + 3];
                int nameEnd = nameBase + nameOff < fst.Length ? Array.IndexOf(fst, (byte)0, nameBase + nameOff) : -1;
                if (nameEnd < 0) throw new InvalidDataException("the disc's file table is damaged");
                string name = System.Text.Encoding.UTF8.GetString(fst, nameBase + nameOff, nameEnd - nameBase - nameOff);
                if (name.Length == 0 || name == "." || name == ".." || name.IndexOfAny(new[] { '/', '\\', ':' }) >= 0 ||
                    name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)
                    throw new InvalidDataException("the disc's file table has an unsafe file name");
                uint a = U32(fst, i * 12 + 4), b = U32(fst, i * 12 + 8);
                if (fst[i * 12] != 0)
                {
                    if (b <= (uint)i || b > (uint)end) throw new InvalidDataException("the disc's file table is damaged");
                    Walk(i + 1, (int)b, Path.Combine(dir, name), depth + 1);
                    i = (int)b;
                }
                else { files.Add((Path.Combine(dir, name), (long)a << 2, b)); i++; }
            }
        }
        Walk(1, entries, "files", 0);

        int total = files.Count + 5, done = 0;
        void Put(string rel, long off, long size)
        {
            cancel.ThrowIfCancellationRequested();
            progress?.Invoke(done, total, rel);
            var root = Path.GetFullPath(outDir).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
            var path = Path.GetFullPath(Path.Combine(outDir, rel));
            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException("the disc's file table has an unsafe file name");
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            using var f = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16);
            for (long o = 0; o < size; o += 1 << 20)
            {
                int n = (int)Math.Min(1 << 20, size - o);
                f.Write(ReadData(off + o, n), 0, n);
            }
            done++;
        }
        Put(Path.Combine("sys", "boot.bin"), 0, 0x440);
        Put(Path.Combine("sys", "bi2.bin"), 0x440, 0x2000);
        Put(Path.Combine("sys", "apploader.img"), 0x2440, aplSize);
        Put(Path.Combine("sys", "main.dol"), dolOff, dolSize);
        Put(Path.Combine("sys", "fst.bin"), fstOff, fstSize);
        foreach (var (path, off, size) in files) Put(path, off, size);
        progress?.Invoke(done, total, "");
        return done;
    }

    // Bytes of the decrypted data partition.
    byte[] ReadData(long off, int count)
    {
        var dst = new byte[count];
        int written = 0;
        while (written < count)
        {
            long ci = Math.DivRem(off + written, ClusterData, out long o);
            if (ci != cachedCluster)
            {
                // RVZ keeps the partition's data decrypted; the other formats store the disc's encrypted clusters.
                cachedPlain = disc.ReadPlainCluster(dataBase + ci * Cluster) ?? Decrypt(disc.Read(dataBase + ci * Cluster, Cluster));
                cachedCluster = ci;
            }
            int n = (int)Math.Min(count - written, ClusterData - o);
            Array.Copy(cachedPlain, (int)o, dst, written, n);
            written += n;
        }
        return dst;
    }

    byte[] Decrypt(byte[] raw) => DecryptCbc(titleKey, raw.AsSpan(0x3D0, 16).ToArray(), raw.AsSpan(0x400, ClusterData));

    static byte[] DecryptCbc(byte[] key, byte[] iv, ReadOnlySpan<byte> data)
    {
        using var aes = Aes.Create();
        aes.Key = key;
        return aes.DecryptCbc(data, iv, PaddingMode.None);
    }

    static uint U32(byte[] b, int o) => BinaryPrimitives.ReadUInt32BigEndian(b.AsSpan(o, 4));

    public void Dispose() => disc.Dispose();

    // The raw (still encrypted) disc bytes behind each container format.
    abstract class Container : IDisposable
    {
        protected readonly FileStream File;
        public int Holes;  // reads that landed on blocks the image does not store (read as zeros)

        protected Container(string path) => File = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.RandomAccess);

        public static Container Open(string path)
        {
            using var f = System.IO.File.OpenRead(path);
            var magic = new byte[4];
            f.ReadExactly(magic, 0, 4);
            var m = System.Text.Encoding.ASCII.GetString(magic);
            if (m == "RVZ\u0001") return new Rvz(path);
            // Dolphin's other compressed formats and NKit images are the usual reason a dump is refused: say how to fix it.
            if (m == "WIA\u0001" || BinaryPrimitives.ReadUInt32LittleEndian(magic) == 0xB10BC001u)
                throw new InvalidDataException("This is a Dolphin WIA or GCZ image. In Dolphin, right-click the game, " +
                                               "choose Convert File, pick RVZ or ISO, and choose the file it makes.");
            var nkit = new byte[4];
            if (f.Length > 0x204) { f.Position = 0x200; f.ReadExactly(nkit, 0, 4); }
            if (System.Text.Encoding.ASCII.GetString(nkit) == "NKIT")
                throw new InvalidDataException("This is an NKit image, which leaves out parts of the disc. Restore it to a full ISO with " +
                                               "NKit's Restore tool (or Dolphin's Convert File to ISO), then choose that.");
            return m == "WBFS" ? new Wbfs(path) : m == "CISO" ? new Ciso(path) : new Iso(path);
        }

        public byte[] Read(long off, int count)
        {
            var dst = new byte[count];
            ReadInto(off, dst);
            return dst;
        }

        protected abstract void ReadInto(long off, Span<byte> dst);

        // The decrypted data of the partition cluster at disc offset off, for formats that store it that way (RVZ).
        public virtual byte[]? ReadPlainCluster(long off) => null;

        protected void ReadFile(long fileOff, Span<byte> dst)
        {
            int n = RandomAccess.Read(File.SafeFileHandle, dst, fileOff);
            if (n < dst.Length) { Holes++; dst[n..].Clear(); }  // past the end of a truncated image
        }

        public void Dispose() => File.Dispose();
    }

    sealed class Iso : Container
    {
        public Iso(string path) : base(path) { }
        protected override void ReadInto(long off, Span<byte> dst) => ReadFile(off, dst);
    }

    // CISO: 0x8000-byte header ("CISO", block size LE, one byte per block: 1 = stored), then the stored blocks in order.
    sealed class Ciso : Container
    {
        readonly long blockSize;
        readonly int[] stored;  // block -> index among stored blocks, or -1
        public Ciso(string path) : base(path)
        {
            var h = new byte[0x8000];
            ReadFile(0, h);
            blockSize = BinaryPrimitives.ReadUInt32LittleEndian(h.AsSpan(4, 4));
            if (blockSize < 0x8000 || blockSize % 0x8000 != 0) throw new InvalidDataException("Unsupported CISO block size.");
            stored = new int[0x8000 - 8];
            for (int i = 0, k = 0; i < stored.Length; i++) stored[i] = h[8 + i] == 1 ? k++ : -1;
        }
        protected override void ReadInto(long off, Span<byte> dst)
        {
            while (dst.Length > 0)
            {
                long i = Math.DivRem(off, blockSize, out long o);
                int n = (int)Math.Min(dst.Length, blockSize - o);
                int idx = i < stored.Length ? stored[i] : -1;
                if (idx < 0) { Holes++; dst[..n].Clear(); }
                else ReadFile(0x8000 + idx * blockSize + o, dst[..n]);
                off += n; dst = dst[n..];
            }
        }
    }

    // WBFS: the first disc slot only (how homebrew dumpers write a single game).
    sealed class Wbfs : Container
    {
        readonly long blockSize;
        readonly ushort[] wlba;
        public Wbfs(string path) : base(path)
        {
            var h = new byte[512];
            ReadFile(0, h);
            long hdSec = 1L << h[8];
            blockSize = 1L << h[9];
            int n = (int)(143432L * 2 * 0x8000 / blockSize);  // table length covers a dual-layer disc
            var table = new byte[0x100 + n * 2];
            ReadFile(hdSec, table);
            wlba = new ushort[n];
            for (int i = 0; i < n; i++) wlba[i] = BinaryPrimitives.ReadUInt16BigEndian(table.AsSpan(0x100 + i * 2, 2));
        }
        protected override void ReadInto(long off, Span<byte> dst)
        {
            while (dst.Length > 0)
            {
                long i = Math.DivRem(off, blockSize, out long o);
                int n = (int)Math.Min(dst.Length, blockSize - o);
                ushort w = i < wlba.Length ? wlba[i] : (ushort)0;
                if (w == 0) { Holes++; dst[..n].Clear(); }
                else ReadFile(w * blockSize + o, dst[..n]);
                off += n; dst = dst[n..];
            }
        }
    }

    // RVZ, Dolphin's compressed format (Dolphin's docs/WiaAndRvz.md; this follows its WIABlob.cpp). The disc is cut
    // into groups of chunk_size bytes, each compressed with Zstandard or stored as is. The areas outside the Wii
    // partition are stored as they are on the disc; the partition's data is stored decrypted and without its hashes,
    // so ReadPlainCluster hands that data to WiiDisc directly and no decryption is needed. Padding the disc generates
    // from a seed is stored as the seed (RVZ packing), regenerated here with the Lagged Fibonacci generator.
    sealed class Rvz : Container
    {
        const int Sector = 0x8000, SectorData = 0x7C00, HashGroupData = 64 * SectorData;

        readonly record struct Group(long FileOffset, int Size, bool Compressed, int PackedSize);
        // Raw: disc bytes [Start, Start + Size). Partition: sectors [Start, Start + Size), stored as 0x7C00 bytes each.
        readonly record struct Area(long Start, long Size, int FirstGroup, int Groups);

        readonly byte[] discHead;
        readonly int compression;  // 0 none, 5 Zstandard (a group can also be stored uncompressed)
        readonly long chunk;       // disc bytes per group
        readonly Area[] raw, partition;
        readonly Group[] groups;
        int cachedGroup = -1;
        byte[] cachedData = Array.Empty<byte>();

        public Rvz(string path) : base(path)
        {
            var h = new byte[0x48 + 0xDC];
            ReadFile(0, h);
            if (BE32(h, 0x0C) < 0xDC) throw Damaged();
            var d = h.AsSpan(0x48);
            compression = (int)BE32(d, 4);
            if (compression is not (0 or 5))
                throw new InvalidDataException("This RVZ file uses " + (compression switch { 2 => "bzip2", 3 => "LZMA", 4 => "LZMA2", _ => "an unknown" }) +
                                               " compression; the launcher reads RVZ with Zstandard (Dolphin's default). In Dolphin, right-click the game, " +
                                               "choose Convert File, and pick RVZ with Zstandard (or ISO).");
            chunk = BE32(d, 12);
            if (chunk < Sector || chunk % Sector != 0 || chunk > 1 << 30) throw Damaged();
            discHead = d.Slice(16, 0x80).ToArray();

            uint parts = BE32(d, 0x90), partSize = BE32(d, 0x94);
            if (parts > 64 || partSize < 48 || partSize > 0x1000) throw Damaged();
            var partBytes = new byte[parts * partSize];
            ReadFile((long)BE64(d, 0x98), partBytes);
            var partAreas = new List<Area>();
            for (int i = 0; i < parts; i++)
                for (int j = 0; j < 2; j++)
                {
                    var e = partBytes.AsSpan(i * (int)partSize + 16 + j * 16, 16);
                    if (BE32(e, 4) > 0) partAreas.Add(new(BE32(e, 0), BE32(e, 4), (int)BE32(e, 8), (int)BE32(e, 12)));
                }
            partition = partAreas.ToArray();

            uint raws = BE32(d, 0xB4);
            if (raws > 4096) throw Damaged();
            var rawBytes = Table(BE64(d, 0xB8), BE32(d, 0xC0), raws * 24);
            raw = new Area[raws];
            for (int i = 0; i < raws; i++)
            {
                var e = rawBytes.AsSpan(i * 24, 24);
                long start = (long)BE64(e, 0), size = (long)BE64(e, 8);
                long skipped = start % Sector;  // the first area starts at 0x80 but holds its whole first sector
                raw[i] = new(start - skipped, size + skipped, (int)BE32(e, 16), (int)BE32(e, 20));
            }

            uint count = BE32(d, 0xC4);
            if (count > 1 << 22) throw Damaged();
            var groupBytes = Table(BE64(d, 0xC8), BE32(d, 0xD0), count * 12);
            groups = new Group[count];
            for (int i = 0; i < count; i++)
            {
                var e = groupBytes.AsSpan(i * 12, 12);
                uint size = BE32(e, 4);
                groups[i] = new((long)BE32(e, 0) << 2, (int)(size & 0x7FFFFFFF), (size & 0x80000000) != 0, (int)BE32(e, 8));
            }
        }

        // The partition's data, decrypted: the 0x7C00 bytes of the sector at disc offset off, or null outside it.
        public override byte[]? ReadPlainCluster(long off)
        {
            long sector = off / Sector;
            foreach (var a in partition)
            {
                if (sector < a.Start || sector >= a.Start + a.Size) continue;
                long groupData = chunk / Sector * SectorData;
                long inArea = (sector - a.Start) * SectorData;
                long g = inArea / groupData;
                if (g >= a.Groups) return null;
                var data = GroupData(a.FirstGroup + (int)g, (int)Math.Min(groupData, a.Size * SectorData - g * groupData),
                                     (int)Math.Max(1, groupData / HashGroupData), g * groupData);
                return data.AsSpan((int)(inArea % groupData), SectorData).ToArray();
            }
            return null;
        }

        protected override void ReadInto(long off, Span<byte> dst)
        {
            while (dst.Length > 0)
            {
                int n;
                if (off < discHead.Length)
                {
                    n = (int)Math.Min(dst.Length, discHead.Length - off);
                    discHead.AsSpan((int)off, n).CopyTo(dst);
                }
                else if (Array.FindIndex(raw, a => off >= a.Start && off < a.Start + a.Size) is var i and >= 0)
                {
                    var a = raw[i];
                    long g = (off - a.Start) / chunk, inGroup = (off - a.Start) % chunk;
                    int length = (int)Math.Min(chunk, a.Size - g * chunk);
                    n = (int)Math.Min(dst.Length, length - inGroup);
                    if (g >= a.Groups) { Holes++; dst[..n].Clear(); }
                    else GroupData(a.FirstGroup + (int)g, length, 0, g * chunk).AsSpan((int)inGroup, n).CopyTo(dst);
                }
                else
                {
                    // Not stored as raw data: up to the next raw area, the partition's data (read through
                    // ReadPlainCluster) or the end of the disc.
                    long next = raw.Where(a => a.Start > off).Select(a => a.Start).DefaultIfEmpty(long.MaxValue).Min();
                    n = (int)Math.Min(dst.Length, next - off);
                    Holes++;
                    dst[..n].Clear();
                }
                off += n;
                dst = dst[n..];
            }
        }

        // A group's data: decompressed, past its hash exception lists (partition groups have one per 2 MiB of data,
        // at least one), and unpacked. junkOffset: where the group starts in its area, which the padding generator uses.
        byte[] GroupData(int index, int length, int exceptionLists, long junkOffset)
        {
            if (index == cachedGroup && cachedData.Length == length) return cachedData;
            if (index < 0 || index >= groups.Length) throw Damaged();
            var g = groups[index];
            var data = new byte[length];
            if (g.Size > 0)  // size 0: every byte is zero and no exceptions
            {
                var stored = new byte[g.Size];
                ReadFile(g.FileOffset, stored);
                // At most the data (packed or not) plus the largest exception lists Dolphin writes.
                long most = Math.Max(length, g.PackedSize) + exceptionLists * (2 + 3328 * 22L) + 4;
                var s = g.Compressed && compression == 5 ? Zstd(stored, most) : stored;
                int p = 0;
                for (int i = 0; i < exceptionLists; i++)
                {
                    if (p + 2 > s.Length) throw Damaged();
                    p += 2 + 22 * BinaryPrimitives.ReadUInt16BigEndian(s.AsSpan(p, 2));
                }
                if (exceptionLists > 0 && !g.Compressed) p = (p + 3) & ~3;  // stored groups pad the lists to 4 bytes
                if (p > s.Length) throw Damaged();
                if (g.PackedSize != 0) Unpack(s.AsSpan(p), data, junkOffset);
                else if (s.Length - p < length) throw Damaged();
                else s.AsSpan(p, length).CopyTo(data);
            }
            cachedGroup = index;
            cachedData = data;
            return data;
        }

        // RVZ packing: runs of (u32 size, data); a size with the top bit set is padding, given by its 68-byte seed.
        static void Unpack(ReadOnlySpan<byte> src, Span<byte> dst, long junkOffset)
        {
            int at = 0;
            while (at < dst.Length)
            {
                if (src.Length < 4) throw Damaged();
                uint size = BinaryPrimitives.ReadUInt32BigEndian(src);
                int n = (int)(size & 0x7FFFFFFF);
                src = src[4..];
                if (n > dst.Length - at) throw Damaged();
                if ((size & 0x80000000) != 0)
                {
                    if (src.Length < 68) throw Damaged();
                    Padding(src[..68], (int)((junkOffset + at) % Sector), dst.Slice(at, n));
                    src = src[68..];
                }
                else
                {
                    if (src.Length < n) throw Damaged();
                    src[..n].CopyTo(dst[at..]);
                    src = src[n..];
                }
                at += n;
            }
        }

        // The discs' padding: a Lagged Fibonacci generator (xor, j = 32, k = 521) from 17 seed words, started skip
        // bytes into its 32 KiB block. Dolphin's LaggedFibonacciGenerator.cpp, including its 18-bit shift.
        static void Padding(ReadOnlySpan<byte> seed, int skip, Span<byte> dst)
        {
            const int K = 521, J = 32, Bytes = K * 4;
            var b = new uint[K];
            for (int i = 0; i < 17; i++) b[i] = BinaryPrimitives.ReadUInt32BigEndian(seed.Slice(i * 4, 4));
            for (int i = 17; i < K; i++) b[i] = (b[i - 17] << 23) ^ (b[i - 16] >> 9) ^ b[i - 1];
            for (int i = 0; i < K; i++) b[i] = (b[i] & 0xFF00FFFF) | ((b[i] >> 2) & 0x00FF0000);
            void Forward()
            {
                for (int i = 0; i < J; i++) b[i] ^= b[i + K - J];
                for (int i = J; i < K; i++) b[i] ^= b[i - J];
            }
            for (int i = 0; i < 4; i++) Forward();
            int pos = skip;
            for (; pos >= Bytes; pos -= Bytes) Forward();
            for (int i = 0; i < dst.Length; i++)
            {
                dst[i] = (byte)(b[pos >> 2] >> (24 - 8 * (pos & 3)));
                if (++pos == Bytes) { Forward(); pos = 0; }
            }
        }

        // A table of the header (raw areas, groups): compressed like the groups, holding at least `size` bytes.
        byte[] Table(ulong off, uint storedSize, long size)
        {
            if (storedSize > 64 << 20 || size > 64 << 20) throw Damaged();
            var stored = new byte[storedSize];
            ReadFile((long)off, stored);
            var t = compression == 5 ? Zstd(stored, size) : stored;
            if (t.Length < size) throw Damaged();
            return t;
        }

        static byte[] Zstd(byte[] stored, long most)
        {
            using var z = new ZstdSharp.DecompressionStream(new MemoryStream(stored));
            using var output = new MemoryStream();
            var buffer = new byte[1 << 16];
            for (int n; (n = z.Read(buffer, 0, buffer.Length)) > 0;)
            {
                if (output.Length + n > most) throw Damaged();
                output.Write(buffer, 0, n);
            }
            return output.ToArray();
        }

        static uint BE32(ReadOnlySpan<byte> b, int o) => BinaryPrimitives.ReadUInt32BigEndian(b.Slice(o, 4));
        static ulong BE64(ReadOnlySpan<byte> b, int o) => BinaryPrimitives.ReadUInt64BigEndian(b.Slice(o, 8));
        static InvalidDataException Damaged() => new("The RVZ file is damaged (or was cut short). Dump or convert it again.");
    }
}
