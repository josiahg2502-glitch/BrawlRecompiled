using System.Formats.Tar;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;

namespace BrawlRecompiled;

// The launcher's updater: asks GitHub for the latest release and installs its download over this folder. The
// player's files stay: engine/UserData (saves, settings, mods, SD card) and the extracted Disc are never in a
// download, and the running launcher is renamed aside first (Windows keeps a running program's file locked).
static class Updater
{
    const string Repo = "josiahg2502-glitch/BrawlRecompiled";

    public sealed record Release(string Version, string AssetUrl, string AssetName);

    static HttpClient Client()
    {
        var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("BrawlRecompiled-Launcher");
        return http;
    }

    // The newer release for this platform, or null (up to date, offline, or no download for this platform).
    public static async Task<Release?> CheckAsync(string currentVersion, bool linux)
    {
        try
        {
            using var http = Client();
            http.Timeout = TimeSpan.FromSeconds(10);
            using var doc = JsonDocument.Parse(await http.GetStringAsync($"https://api.github.com/repos/{Repo}/releases/latest"));
            string tag = doc.RootElement.GetProperty("tag_name").GetString() ?? "";
            if (!Version.TryParse(tag.TrimStart('v'), out var latest) || !Version.TryParse(currentVersion, out var current) || latest <= current)
                return null;
            string suffix = linux ? "-Linux.tar.gz" : "-Windows.zip";
            foreach (var asset in doc.RootElement.GetProperty("assets").EnumerateArray())
            {
                string name = asset.GetProperty("name").GetString() ?? "";
                if (name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                    return new Release(tag.TrimStart('v'), asset.GetProperty("browser_download_url").GetString()!, name);
            }
        }
        catch (Exception) { }
        return null;
    }

    // The folder a release unpacks to: the launcher's own, when the game sits in its engine folder (not the dev tree).
    public static string? InstallRoot(string engineExeName)
    {
        string root = AppContext.BaseDirectory;
        return File.Exists(Path.Combine(root, "engine", engineExeName)) ? root : null;
    }

    // Downloads and unpacks the release over `root`; `progress` gets 0..100. Throws on failure, leaving the old files
    // in place for every file not yet written.
    public static async Task InstallAsync(Release release, string root, Action<int>? progress = null)
    {
        string temp = Path.Combine(root, "update.download");
        try
        {
            using (var http = Client())
            using (var response = await http.GetAsync(release.AssetUrl, HttpCompletionOption.ResponseHeadersRead))
            {
                response.EnsureSuccessStatusCode();
                long total = response.Content.Headers.ContentLength ?? 0;
                await using var input = await response.Content.ReadAsStreamAsync();
                await using var output = File.Create(temp);
                var buffer = new byte[1 << 16];
                long done = 0;
                int reported = -1;
                for (int n; (n = await input.ReadAsync(buffer)) > 0;)
                {
                    await output.WriteAsync(buffer.AsMemory(0, n));
                    done += n;
                    int percent = total > 0 ? (int)(done * 90 / total) : 0;
                    if (percent != reported) progress?.Invoke(reported = percent);
                }
            }
            await Task.Run(() =>
            {
                if (release.AssetName.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                {
                    using var zip = ZipFile.OpenRead(temp);
                    foreach (var entry in zip.Entries)
                    {
                        if (Target(root, entry.FullName) is not { } path) continue;
                        if (entry.FullName.EndsWith('/')) { Directory.CreateDirectory(path); continue; }
                        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                        MoveAsideIfRunning(path);
                        entry.ExtractToFile(path, overwrite: true);
                    }
                }
                else
                {
                    using var file = File.OpenRead(temp);
                    using var gz = new GZipStream(file, CompressionMode.Decompress);
                    using var tar = new TarReader(gz);
                    while (tar.GetNextEntry() is { } entry)
                    {
                        if (Target(root, entry.Name) is not { } path) continue;
                        if (entry.EntryType == TarEntryType.Directory) { Directory.CreateDirectory(path); continue; }
                        if (entry.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile)) continue;
                        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                        MoveAsideIfRunning(path);
                        entry.ExtractToFile(path, overwrite: true);  // keeps the programs' executable bit
                    }
                }
            });
            progress?.Invoke(100);
        }
        finally
        {
            try { File.Delete(temp); } catch (Exception) { }
        }
    }

    // Where an archive entry goes: its path without the top folder (BrawlRecompiled/), or null for what must not be
    // replaced (the player's UserData) and anything that would land outside the install.
    static string? Target(string root, string entryName)
    {
        string name = entryName.Replace('\\', '/');
        int slash = name.IndexOf('/');
        if (slash < 0) return null;
        string rel = name[(slash + 1)..];
        if (rel.Length == 0 || rel.StartsWith("engine/UserData/", StringComparison.OrdinalIgnoreCase) && rel != "engine/UserData/")
            return null;
        string full = Path.GetFullPath(Path.Combine(root, rel));
        string rootFull = Path.GetFullPath(root);
        return full.StartsWith(rootFull, StringComparison.OrdinalIgnoreCase) ? full : null;
    }

    // The running launcher cannot be overwritten on Windows, but it can be renamed; the next start removes it.
    static void MoveAsideIfRunning(string path)
    {
        if (!string.Equals(path, Environment.ProcessPath, StringComparison.OrdinalIgnoreCase) || !File.Exists(path)) return;
        string old = path + ".old";
        if (File.Exists(old)) File.Delete(old);
        File.Move(path, old);
    }

    // After an update: the previous launcher's renamed file.
    public static void CleanUp()
    {
        try { if (Environment.ProcessPath is { } exe && File.Exists(exe + ".old")) File.Delete(exe + ".old"); } catch (Exception) { }
    }
}
