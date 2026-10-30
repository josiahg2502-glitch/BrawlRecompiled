using System.Buffers.Binary;
using System.Security.Cryptography;

namespace BrawlRecompiled;

// A Wii disc image (.iso, .ciso or .wbfs): decodes the container, decrypts the data partition and extracts it
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
                var raw = disc.Read(dataBase + ci * Cluster, Cluster);
                cachedPlain = DecryptCbc(titleKey, raw.AsSpan(0x3D0, 16).ToArray(), raw.AsSpan(0x400, ClusterData));
                cachedCluster = ci;
            }
            int n = (int)Math.Min(count - written, ClusterData - o);
            Array.Copy(cachedPlain, (int)o, dst, written, n);
            written += n;
        }
        return dst;
    }

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
            // Dolphin's compressed formats and NKit images are the usual reason a dump is refused: say how to fix it.
            if (m is "RVZ\u0001" or "WIA\u0001" || BinaryPrimitives.ReadUInt32LittleEndian(magic) == 0xB10BC001u)
                throw new InvalidDataException("This is a Dolphin compressed image (RVZ, WIA or GCZ). In Dolphin, right-click the game, " +
                                               "choose Convert File, pick ISO, and choose the .iso it makes.");
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
}
