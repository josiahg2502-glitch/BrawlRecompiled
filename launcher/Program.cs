namespace BrawlRecompiled;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // Headless self-test / tooling: "--extract <image> <outdir>" writes <outdir>\extract.log and exits 0 on success.
        if (args.Length == 3 && args[0] == "--extract")
        {
            var log = new List<string>();
            try
            {
                using var disc = WiiDisc.Open(args[1]);
                log.Add($"game {disc.GameId} rev {disc.Revision} \"{disc.Title}\" problem=\"{disc.Problem()}\"");
                if (disc.Problem().Length > 0) { Directory.CreateDirectory(args[2]); File.WriteAllLines(Path.Combine(args[2], "extract.log"), log); return 3; }
                int n = disc.Extract(args[2], (done, total, name) => { if (done % 500 == 0) log.Add($"{done}/{total} {name}"); });
                log.Add($"extracted {n} files, holes {disc.Holes}");
                Directory.CreateDirectory(args[2]);
                File.WriteAllLines(Path.Combine(args[2], "extract.log"), log);
                return disc.Holes == 0 ? 0 : 2;
            }
            catch (Exception e)
            {
                log.Add("ERROR " + e);
                Directory.CreateDirectory(args[2]);
                File.WriteAllLines(Path.Combine(args[2], "extract.log"), log);
                return 1;
            }
        }
        // "--import-save <data.bin or a file in a Dolphin save folder> <data dir>": headless save import (exit 0 ok, 1 error).
        if (args.Length == 3 && args[0] == "--import-save")
        {
            try { Console.WriteLine(string.Join(" ", WiiSave.Import(args[1], args[2]))); return 0; }
            catch (Exception e) { Console.Error.WriteLine(e.Message); return 1; }
        }
        // "--savekey <extracted disc> <out file>": the per-disc save key (matches the runtime's UserData\Saves\<key>).
        if (args.Length == 3 && args[0] == "--savekey")
        {
            File.WriteAllText(args[2], WiiSave.GameSaveKey(args[1]));
            return 0;
        }
        // "--savetest <save folder> <scratch dir>": the folder's files survive a data.bin round trip (exit 0 ok, 1 not).
        if (args.Length == 3 && args[0] == "--savetest")
        {
            var files = Directory.GetFiles(args[1]).ToDictionary(Path.GetFileName, File.ReadAllBytes)!;
            Directory.CreateDirectory(args[2]);
            var bin = Path.Combine(args[2], "data.bin");
            File.WriteAllBytes(bin, WiiSave.WriteDataBin(files!));
            var back = WiiSave.ReadDataBin(File.ReadAllBytes(bin));
            return back.Count == files.Count && files.All(f => back.TryGetValue(f.Key!, out var b) && b.AsSpan().SequenceEqual(f.Value)) ? 0 : 1;
        }
        ApplicationConfiguration.Initialize();
        // "--screenshot <dir>" draws every page to <dir>\<page>.png and exits (visual check without the desktop).
        int shot = Array.IndexOf(args, "--screenshot");
        bool selfTest = args.Contains("--selftest"), update = args.Contains("--update");
        var form = new MainForm(selfTest, shot >= 0 && shot + 1 < args.Length ? args[shot + 1] : null, update);
        // Normally no window: the game starts, and its settings are in its own options menu. This window shows for the
        // first start (choosing the disc), an update (--update, from the game) or --settings.
        if (!selfTest && shot < 0 && !update && !args.Contains("--settings") && form.TryStartGame()) return 0;
        Application.Run(form);
        return 0;
    }
}
