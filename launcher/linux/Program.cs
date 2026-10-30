using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Media;
using Avalonia.Themes.Fluent;

namespace BrawlRecompiled;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // The Windows launcher's headless modes (launcher\Program.cs), same arguments.
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
        if (args.Length == 3 && args[0] == "--import-save")
        {
            try { Console.WriteLine(string.Join(" ", WiiSave.Import(args[1], args[2]))); return 0; }
            catch (Exception e) { Console.Error.WriteLine(e.Message); return 1; }
        }
        return AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont()
            .With(new FontManagerOptions { DefaultFamilyName = "fonts:Inter#Inter" })
            .StartWithClassicDesktopLifetime(args);
    }
}

sealed class App : Application
{
    public override void Initialize()
    {
        Styles.Add(new FluentTheme());
        RequestedThemeVariant = Avalonia.Styling.ThemeVariant.Light;
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var args = desktop.Args ?? Array.Empty<string>();
            int shot = Array.IndexOf(args, "--screenshot");
            // "--screenshot <dir>" draws every page to <dir>/<page>.png and exits (visual check without the desktop).
            bool selfTest = args.Contains("--selftest"), update = args.Contains("--update");
            var window = new MainWindow(selfTest, shot >= 0 && shot + 1 < args.Length ? args[shot + 1] : null, update);
            // Normally no window: the game starts, and its settings are in its own options menu. This window shows for
            // the first start (choosing the disc), an update (--update, from the game) or --settings.
            if (!selfTest && shot < 0 && !update && !args.Contains("--settings") && window.TryStartGame()) Environment.Exit(0);
            desktop.MainWindow = window;
        }
        base.OnFrameworkInitializationCompleted();
    }
}
