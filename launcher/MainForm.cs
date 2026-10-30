using System.Diagnostics;
using System.Drawing.Drawing2D;

namespace BrawlRecompiled;

// The launcher window: a dark header with the title, a nav column (Play, Display, Controls, Mods) and one card per
// page. Everything it changes goes to <game folder>\UserData\Config.toml (engine\ in a release, Play\ in the dev
// tree), read by the game at start.
sealed class MainForm : Form
{
    static readonly Color Night = ColorTranslator.FromHtml("#1A1E2C"), Card = ColorTranslator.FromHtml("#262B3C"),
        Snow = ColorTranslator.FromHtml("#F2F2F6"), Dim = ColorTranslator.FromHtml("#B8BCCB"), Orange = ColorTranslator.FromHtml("#FF8C28"),
        Red = ColorTranslator.FromHtml("#D83A3A"), Edge = ColorTranslator.FromHtml("#3C4358"), Hover = ColorTranslator.FromHtml("#343A50"),
        Gold = ColorTranslator.FromHtml("#FFC43A");

    readonly bool selfTest;
    readonly string? screenshotDir;
    readonly bool update;  // --update: the game asked for the new release to be installed
    // Fonts are in points and grow with Windows display scaling; every pixel size goes through S() so it grows too.
    static float k = 1f;
    static int S(int v) => (int)Math.Round(v * k);
    static Size S(int w, int h) => new(S(w), S(h));
    static Padding S(int l, int t, int r, int b) => new(S(l), S(t), S(r), S(b));
    Button chooseDisc = null!;
    // play = the folder holding the game exe and its UserData: engine\ in a release, Play\ in the dev tree.
    readonly string root, play, gameExe, userData, configPath, modsDir, sdDir, discDir;
    // The one version number: <Version> in BrawlRecompiled.Launcher.csproj (package-brawl.ps1 names the zip after it).
    static readonly string AppVersion = typeof(MainForm).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";
    readonly GameConfig config;
    readonly Panel content = new() { Dock = DockStyle.Fill };
    readonly List<(Button button, Control page)> pages = new();
    Label playStatus = null!, dumpHelp = null!, gameNote = null!, sdStatus = null!;
    ListView modList = null!;
    Func<bool>? cancelPadCapture;  // the Controls page's gamepad capture: Escape ends it

    public MainForm(bool selfTest, string? screenshotDir = null, bool update = false)
    {
        this.selfTest = selfTest;
        this.update = update;
        this.screenshotDir = screenshotDir;
        k = DeviceDpi / 96f;
        content.Padding = S(16, 12, 20, 16);
        (root, play, gameExe) = FindGame();
        // Zips drop the hidden attribute: keep the release's engine\ folder out of sight so only this exe shows.
        try
        {
            var engineDir = new DirectoryInfo(play);
            if (engineDir.Exists && engineDir.Name.Equals("engine", StringComparison.OrdinalIgnoreCase) && !engineDir.Attributes.HasFlag(FileAttributes.Hidden))
                engineDir.Attributes |= FileAttributes.Hidden;
        }
        catch (Exception) { }  // read-only media or no rights: cosmetic only
        userData = Path.Combine(play, "UserData");
        configPath = Path.Combine(userData, "Config.toml");
        modsDir = Path.Combine(userData, "mods");
        sdDir = Path.Combine(userData, "SD");
        discDir = Path.Combine(root, "Disc", "DATA");
        config = new GameConfig(configPath);
        EnsureDefaults();
        Gamepads.Init(play);

        Text = "Brawl Recompiled  v" + AppVersion;
        Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!);  // the Smash Ball, embedded from smash.ico
        ballCursor = MakeBallCursor(clickable: false);
        ballHand = MakeBallCursor(clickable: true);
        Cursor = ballCursor;  // child controls without their own cursor inherit it
        var area = Screen.PrimaryScreen?.WorkingArea.Size ?? new Size(S(1100), S(760));
        ClientSize = new Size(Math.Min(S(1100), area.Width - 40), Math.Min(S(760), area.Height - 60));
        MinimumSize = new Size(Math.Min(S(940), area.Width), Math.Min(S(640), area.Height));
        BackColor = Night;
        ForeColor = Snow;
        Font = new Font("Segoe UI", 10.5f);
        KeyPreview = true;
        StartPosition = FormStartPosition.CenterScreen;
        DoubleBuffered = true;

        var header = new DoubleBufferedPanel { Dock = DockStyle.Top, Height = S(150) };
        header.Paint += PaintHeader;
        var nav = new BufferedFlow { Dock = DockStyle.Left, Width = S(220), FlowDirection = FlowDirection.TopDown, Padding = S(16, 18, 0, 0), WrapContents = false };
        nav.Paint += PaintNav;
        // Click Mario or Sonic to make them hop, the Smash Ball for another tip, the fighter under the menu to swap them in.
        header.MouseClick += (_, e) =>
        {
            if (leftRect.Contains(e.Location)) jumpAt["left"] = T;
            else if (rightRect.Contains(e.Location)) jumpAt["right"] = T;
            else if (ballRect.Contains(e.Location)) NextTip();
        };
        header.MouseMove += (_, e) => header.Cursor = leftRect.Contains(e.Location) || rightRect.Contains(e.Location) || ballRect.Contains(e.Location) ? ballHand : ballCursor;
        nav.MouseClick += (_, e) => { if (navRect.Contains(e.Location) && Hop("nav", 0.9) < 0) jumpAt["nav"] = T; };
        nav.MouseMove += (_, e) => nav.Cursor = navRect.Contains(e.Location) ? ballHand : ballCursor;
        var frame = new System.Windows.Forms.Timer { Interval = 33 };
        frame.Tick += (_, _) =>
        {
            if (!animate || WindowState == FormWindowState.Minimized) return;
            header.Invalidate();
            nav.Invalidate(new Rectangle(0, navSpriteTop, nav.Width, Math.Max(0, nav.Height - navSpriteTop)));
            float glow = (float)(Math.Sin(T * 3) + 1) / 2;  // PLAY breathes between two reds
            playButton.BackColor = Color.FromArgb(Lerp(Red.R, 255, glow), Lerp(Red.G, 100, glow), Lerp(Red.B, 70, glow));
        };
        frame.Start();
        Controls.Add(content);
        Controls.Add(nav);
        Controls.Add(header);

        AddPage(nav, "Play", BuildPlayPage());
        AddPage(nav, "Display", BuildDisplayPage());
        AddPage(nav, "Controls", BuildControlsPage());
        AddPage(nav, "Mods", BuildModsPage());
        AddPage(nav, "Online", BuildOnlinePage());
        ShowPage(pages[0].button);
        Shown += (_, _) =>
        {
            if (screenshotDir != null)
            {
                // Headless visual check: every page drawn to <dir>\<page>.png.
                Directory.CreateDirectory(screenshotDir);
                foreach (var p in pages)
                {
                    ShowPage(p.button);
                    Application.DoEvents();
                    using var bmp = new Bitmap(Width, Height);
                    DrawToBitmap(bmp, new Rectangle(Point.Empty, Size));
                    bmp.Save(Path.Combine(screenshotDir, p.button.Text.Replace(' ', '_') + ".png"));
                }
                Close();
            }
            else if (update) _ = CheckForUpdateAsync();
            else { foreach (var p in pages) ShowPage(p.button); Environment.ExitCode = !File.Exists(gameExe) ? 6 : !Gamepads.Ready ? 4 : Gamepads.Connected().All(p => Gamepads.Opens(p.id)) ? 0 : 5; Close(); }
        };
    }

    // #26: the game finds a newer release (its options menu, System) and starts this with --update to install it over
    // this folder, then the game starts again.
    async Task CheckForUpdateAsync()
    {
        Updater.CleanUp();
        if (Updater.InstallRoot(EngineExe) is not { } installRoot || await Updater.CheckAsync(AppVersion, linux: false) is not { } release)
        {
            if (TryStartGame()) Close();
            return;
        }
        // The game closes itself after starting this; its files can't be replaced while it runs.
        foreach (var running in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(EngineExe))) running.WaitForExit(30000);
        string title = Text;
        Enabled = false;
        UseWaitCursor = true;
        try
        {
            await Updater.InstallAsync(release, installRoot, p => BeginInvoke(() => Text = $"Updating to v{release.Version}...  {p}%"));
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false });
            Close();
        }
        catch (Exception e)
        {
            Enabled = true;
            UseWaitCursor = false;
            Text = title;
            MessageBox.Show(this, "The update could not be installed:\n" + e.Message + "\n\nDownload it from github.com/josiahg2502-glitch/BrawlRecompiled/releases instead.",
                "Update", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    // The release keeps the game in engine\ (renamed so nobody double-clicks it); the dev tree keeps it in Play\.
    // Search up from the launcher's own folder, so launcher\publish\ works in the dev tree too.
    public const string EngineExe = "brawl-engine.exe";
    static (string root, string dir, string exe) FindGame()
    {
        var start = new DirectoryInfo(AppContext.BaseDirectory);
        for (var d = start; d != null; d = d.Parent)
        {
            foreach (var (dir, exe) in new[] { ("engine", EngineExe), ("Play", "SuperSmashBrosBrawl.exe") })
                if (File.Exists(Path.Combine(d.FullName, dir, exe))) return (d.FullName, Path.Combine(d.FullName, dir), Path.Combine(d.FullName, dir, exe));
        }
        return (start.FullName, Path.Combine(start.FullName, "engine"), Path.Combine(start.FullName, "engine", EngineExe));
    }

    // This disc's save folder (per extracted disc, as the game keeps them).
    string SaveDataDir() => WiiSave.DataDir(userData, DiscStatus().path, WiiDisc.Game);

    // The packaged defaults: borderless 1080p, the game's own 16:9, 120 fps frame interpolation (shown on screens faster
    // than 60 Hz). Keys the user already has are left alone.
    void EnsureDefaults()
    {
        bool changed = false;
        foreach (var (key, value) in new[] { ("display_mode", "\"borderless\""), ("resolution_multiplier", "2.25"), ("widescreen", "true"), ("aspect_mode", "1"),
                     ("frame_interpolation_fps", "120"), ("texture_replacements", "true"), ("vsync", "true") })
            if (config.Get("video", key) == null) { config.Set("video", key, value); changed = true; }
        if (changed) config.Save();
    }

    void AddPage(FlowLayoutPanel nav, string title, Control page)
    {
        // The title is drawn in the game's font (NavTitle); Text stays for the screenshot names and accessibility.
        var button = new Button
        {
            Text = title, Size = S(188, 50), FlatStyle = FlatStyle.Flat, BackColor = Card, ForeColor = Card,
            Font = new Font("Segoe UI", 1f), ImageAlign = ContentAlignment.MiddleLeft, Padding = S(10, 0, 0, 0),
            Margin = S(0, 0, 0, 10), Cursor = ballHand, UseCompatibleTextRendering = false,
            Image = NavTitle(title, false), AccessibleName = title,
        };
        button.FlatAppearance.BorderColor = Edge;
        button.FlatAppearance.BorderSize = S(2);
        button.Click += (_, _) => ShowPage(button);
        button.MouseEnter += (_, _) => { if (button.BackColor == Card) button.BackColor = button.ForeColor = Hover; };
        button.MouseLeave += (_, _) => { if (button.BackColor == Hover) button.BackColor = button.ForeColor = Card; };
        page.Dock = DockStyle.Fill;
        page.Visible = false;
        content.Controls.Add(page);
        nav.Controls.Add(button);
        pages.Add((button, page));
    }

    void ShowPage(Button button)
    {
        foreach (var (b, page) in pages)
        {
            bool on = b == button;
            b.BackColor = on ? Orange : Card;
            b.ForeColor = b.BackColor;  // the text is the image; the button's own text stays out of sight
            b.Image = NavTitle(b.Text, on);
            page.Visible = on;
        }
        RefreshStatus();
    }

    // A menu entry's title in the game's font: white on the dark buttons, dark on the selected orange one.
    static Bitmap NavTitle(string title, bool selected)
    {
        Bitmap Draw(int height) => selected
            ? GameArt.Text(title.ToUpperInvariant(), height, Night, ColorTranslator.FromHtml("#3A2410"), Gold)
            : GameArt.Text(title.ToUpperInvariant(), height, Snow, Dim, ColorTranslator.FromHtml("#0B0D16"));
        // Long titles ("CONTROLS") shrink to fit the button.
        var image = Draw(S(26));
        int room = S(166);
        return image.Width <= room ? image : Draw(S(26) * room / image.Width);
    }

    // ----- The header: the title over a dark sky with drifting embers -----
    // Everything moves on one clock; with Windows animations turned off the scene stands still.
    readonly bool animate = SystemInformation.UIEffectsEnabled;
    readonly Stopwatch clock = Stopwatch.StartNew();
    double T => animate ? clock.Elapsed.TotalSeconds : 1.0;
    Bitmap? headerTitle;
    Button playButton = null!;
    readonly Dictionary<string, double> jumpAt = new();
    readonly Dictionary<(string, int, int, bool), Bitmap> scaled = new();
    Rectangle leftRect, rightRect, ballRect, navRect;
    int navSpriteTop;
    // The fighters who take turns under the menu (a click spins the current one out and the next one in).
    static readonly string[] NavFighters = { "kirby", "pikachu", "fox", "samus", "pit", "metaknight", "ike", "snake" };
    int navFighter;

    static int Lerp(int a, int b, float f) => (int)(a + (b - a) * f);

    // How far into a click-triggered move (0..1), or -1 when none is playing.
    double Hop(string name, double seconds) => jumpAt.TryGetValue(name, out var at) && T - at < seconds ? (T - at) / seconds : -1;

    // A piece of art resized once and kept (drawing a pre-sized bitmap every frame is cheap).
    Bitmap Sprite(string name, int w, int h, bool flip = false)
    {
        if (scaled.TryGetValue((name, w, h, flip), out var b)) return b;
        b = new Bitmap(Math.Max(1, w), Math.Max(1, h));
        using (var g = Graphics.FromImage(b))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            if (GameArt.Image(name) is { } img) g.DrawImage(img, new Rectangle(0, 0, w, h));
        }
        if (flip) b.RotateFlip(RotateFlipType.RotateNoneFlipX);
        return scaled[(name, w, h, flip)] = b;
    }

    void BuildHeaderTitle(Size size)
    {
        headerTitle?.Dispose();
        var r = new Rectangle(Point.Empty, size);
        headerTitle = new Bitmap(size.Width, size.Height);
        using var g = Graphics.FromImage(headerTitle);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using (var pen = new Pen(Orange, S(4))) g.DrawLine(pen, 0, r.Height - S(2), r.Width, r.Height - S(2));
        // The title in the game's own title font, shrunk if the window is narrow so it stays clear of the fighters.
        int height = Math.Min(S(58), (r.Width - S(2 * 200)) / 9);
        var title = GameArt.Text("BRAWL RECOMPILED", height, ColorTranslator.FromHtml("#FFF3B0"), Orange, Color.FromArgb(255, 12, 14, 22));
        g.DrawImage(title, (r.Width - title.Width) / 2, S(20));
        using var sub = new Font("Segoe UI", 10.5f, FontStyle.Bold);
        var fmt = new StringFormat { Alignment = StringAlignment.Center };
        var subText = "Super Smash Bros. Brawl (Wii) on PC  -  made by JosiahG  -  v" + AppVersion;
        var subSize = g.MeasureString(subText, sub);
        var subRect = new RectangleF(r.Width / 2f - subSize.Width / 2 - S(10), S(26) + title.Height, subSize.Width + S(20), subSize.Height + S(2));
        using (var tag = Rounded(Rectangle.Round(subRect), S(8))) { g.FillPath(new SolidBrush(Card), tag); g.DrawPath(new Pen(Edge, S(2)), tag); }
        g.DrawString(subText, sub, new SolidBrush(Snow), new PointF(r.Width / 2f, subRect.Y + S(1)), fmt);
    }

    // A four-point sparkle, as the Smash Ball's trail.
    static void Sparkle(Graphics g, float cx, float cy, float r, Color color)
    {
        float n = r * 0.3f;
        using var star = new GraphicsPath();
        star.AddPolygon(new[] { new PointF(cx, cy - r), new PointF(cx + n, cy - n), new PointF(cx + r, cy), new PointF(cx + n, cy + n),
                                new PointF(cx, cy + r), new PointF(cx - n, cy + n), new PointF(cx - r, cy), new PointF(cx - n, cy - n) });
        using var fill = new SolidBrush(color);
        g.FillPath(fill, star);
    }

    void PaintHeader(object? sender, PaintEventArgs e)
    {
        var g = e.Graphics;
        var r = ((Control)sender!).ClientRectangle;
        if (r.Width <= 0 || r.Height <= 0) return;
        if (headerTitle == null || headerTitle.Size != r.Size) BuildHeaderTitle(r.Size);
        double t = T;
        using (var sky = new LinearGradientBrush(r, ColorTranslator.FromHtml("#0E1120"), Card, LinearGradientMode.Vertical)) g.FillRectangle(sky, r);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        // Embers rise and fade, each on its own lane.
        for (int i = 0; i < 24; i++)
        {
            double life = (t * (0.12 + 0.04 * (i % 5)) + i * 0.37) % 1.0;
            float x = (float)((i * 97 % 100) / 100.0 * r.Width + Math.Sin(t * 1.3 + i) * S(8));
            float y = (float)(r.Height - life * r.Height);
            int size = S(3 + i % 3);
            using var ember = new SolidBrush(Color.FromArgb((int)(170 * (1 - life)), i % 2 == 0 ? Orange : Gold));
            g.FillEllipse(ember, x, y, size, size);
        }
        g.DrawImageUnscaled(headerTitle!, 0, 0);
        g.InterpolationMode = InterpolationMode.Bilinear;

        // The Smash Ball drifts across the sky, spinning, trailing sparkles; a click on it shows another tip.
        double half = r.Width / 2.0 - S(230);
        PointF At(double when) => new((float)(r.Width / 2.0 + Math.Sin(when * 0.42) * half), (float)(S(42) + Math.Sin(when * 1.2) * S(18)));
        for (int k = 7; k >= 1; k--)
        {
            var p = At(t - k * 0.08);
            Sparkle(g, p.X + (float)Math.Sin(t * 7 + k) * S(4), p.Y + (float)Math.Cos(t * 5 + k) * S(4), S(12 - k), Color.FromArgb(230 - k * 28, k % 2 == 0 ? Gold : Color.White));
        }
        var bp = At(t);
        int ball = S(46);
        ballRect = new Rectangle((int)bp.X - ball / 2, (int)bp.Y - ball / 2, ball, ball);
        var state = g.Save();
        g.TranslateTransform(bp.X, bp.Y);
        g.RotateTransform((float)(Math.Sin(t * 0.9) * 25));
        g.DrawImage(Sprite("smash_ball", ball, ball), -ball / 2, -ball / 2);
        g.Restore(state);

        // Mario and Sonic bounce at the sides; a click makes one hop.
        float Lift(string name, double phase)
        {
            double hop = Hop(name, 0.7);
            return (float)(Math.Abs(Math.Sin(t * 2.2 + phase)) * S(5) + (hop < 0 ? 0 : Math.Sin(hop * Math.PI) * S(30)));
        }
        int w = S(104), h = S(130);
        leftRect = new Rectangle(S(18), r.Height - h + S(10) - (int)Lift("left", 0), w, h);
        rightRect = new Rectangle(r.Width - w - S(18), r.Height - h + S(10) - (int)Lift("right", 1.3), w, h);
        g.SetClip(new Rectangle(0, 0, r.Width, r.Height - S(4)));
        g.DrawImage(Sprite("fighter_mario", w, h), leftRect.Location);
        g.DrawImage(Sprite("fighter_sonic", w, h, flip: true), rightRect.Location);
        g.ResetClip();
    }

    // A fighter stands under the menu buttons when the window is tall enough; a click spins them round and the next
    // fighter turns in.
    void PaintNav(object? sender, PaintEventArgs e)
    {
        var nav = (Control)sender!;
        var g = e.Graphics;
        double t = T;
        navSpriteTop = pages.Count > 0 ? pages[^1].button.Bottom + S(10) : 0;
        int bottom = nav.ClientSize.Height - S(10);
        int room = bottom - navSpriteTop - S(12);
        if (room < S(110)) { navRect = Rectangle.Empty; return; }
        int h = Math.Min(S(200), room), w = h * 4 / 5;
        double spin = Hop("nav", 0.9);
        if (spin < 0 && jumpAt.ContainsKey("nav")) { jumpAt.Remove("nav"); }
        // The spin squeezes them to a sliver (a turn), and the next fighter comes round from the halfway point.
        double turn = spin < 0 ? 1 : Math.Cos(spin * Math.PI * 2);
        if (spin >= 0.5 && navTurned == false) { navFighter = (navFighter + 1) % NavFighters.Length; navTurned = true; }
        if (spin < 0) navTurned = false;
        int sw = Math.Max(1, (int)(w * Math.Abs(turn)));
        int y = bottom - h - S(12) - (int)((Math.Sin(t * 1.6) + 1) * S(5));
        int cx = (nav.Width + S(16)) / 2;
        navRect = new Rectangle(cx - w / 2, y, w, h);
        int shadowW = w * 2 / 3 - (int)((Math.Sin(t * 1.6) + 1) * S(8));
        using (var shadow = new SolidBrush(Color.FromArgb(70, 0, 0, 0)))
            g.FillEllipse(shadow, cx - shadowW / 2, bottom - S(10), shadowW, S(12));
        g.InterpolationMode = InterpolationMode.Bilinear;
        g.DrawImage(Sprite("fighter_" + NavFighters[navFighter], w, h, turn < 0), new Rectangle(cx - sw / 2, y, sw, h));
    }
    bool navTurned;

    // ----- The pointer: the Smash Ball (a little smaller with a sparkle over anything clickable) -----
    static Cursor ballCursor = Cursors.Default, ballHand = Cursors.Hand;

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    struct IconInfo { public bool fIcon; public int xHotspot, yHotspot; public IntPtr hbmMask, hbmColor; }
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern IntPtr CreateIconIndirect(ref IconInfo info);
    [System.Runtime.InteropServices.DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);

    Cursor MakeBallCursor(bool clickable)
    {
        if (GameArt.Image("smash_ball") is not { } img) return clickable ? Cursors.Hand : Cursors.Default;
        int size = S(32);
        using var bmp = new Bitmap(size, size);
        using (var g = Graphics.FromImage(bmp))
        {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            int ball = clickable ? size * 3 / 4 : size * 7 / 8;
            g.DrawImage(img, new Rectangle(0, 0, ball, ball));
            if (clickable) Sparkle(g, size * 0.8f, size * 0.8f, size * 0.2f, Gold);
        }
        // The hotspot is the ball's upper-left edge, where it points.
        var info = new IconInfo { fIcon = false, xHotspot = size / 7, yHotspot = size / 7, hbmColor = bmp.GetHbitmap(Color.FromArgb(0)), hbmMask = new Bitmap(size, size).GetHbitmap() };
        var handle = CreateIconIndirect(ref info);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        return handle == IntPtr.Zero ? Cursors.Default : new Cursor(handle);
    }

    // A card holding one page's controls, top to bottom.
    static (Panel card, FlowLayoutPanel body) NewCard(string heading)
    {
        var card = new DoubleBufferedPanel { Padding = S(22, 16, 22, 16), BackColor = Night };
        card.Paint += (s, e) =>
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var r = ((Control)s!).ClientRectangle;
            r.Width -= S(8); r.Height -= S(8);
            r.Offset(S(2), S(2));
            using var path = Rounded(r, S(14));
            g.FillPath(new SolidBrush(Card), path);
            g.DrawPath(new Pen(Edge, S(3)), path);
        };
        // The body sizes to its content and the card scrolls only when that is taller than the card.
        var body = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Card };
        card.AutoScroll = true;
        // The heading in the game's title font.
        var title = GameArt.Text(heading.ToUpperInvariant(), S(38), Color.White, ColorTranslator.FromHtml("#FFC86A"), ColorTranslator.FromHtml("#0B0D16"));
        body.Controls.Add(new PictureBox { Image = title, Size = title.Size, Margin = S(0, 0, 0, 8), AccessibleName = heading });
        card.Controls.Add(body);
        return (card, body);
    }

    static GraphicsPath Rounded(Rectangle r, int radius)
    {
        var p = new GraphicsPath();
        int d = radius * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    static Label Note(string text, int width = 700) => new()
    {
        Text = text, AutoSize = true, MaximumSize = new Size(S(width), 0), ForeColor = Dim, Margin = S(0, 6, 0, 6),
    };

    static Button Btn(string text, EventHandler onClick, Color? back = null)
    {
        var fill = back ?? Orange;
        var b = new Button
        {
            Text = text, AutoSize = true, FlatStyle = FlatStyle.Flat, BackColor = fill, ForeColor = fill == Orange || fill == Gold ? Night : Snow,
            Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Padding = S(10, 4, 10, 4), Margin = S(0, 6, 10, 6), Cursor = ballHand,
        };
        b.FlatAppearance.BorderColor = fill == Card ? Edge : fill;
        b.FlatAppearance.BorderSize = S(2);
        b.Click += onClick;
        return b;
    }

    // ----- Play -----
    Control BuildPlayPage()
    {
        var (card, body) = NewCard("Ready to brawl?");
        playButton = Btn("", (_, _) => Launch(), Red);
        playButton.Image = GameArt.Text("PLAY", S(52), Color.White, ColorTranslator.FromHtml("#FFE08A"), ColorTranslator.FromHtml("#5A0E0E"));
        playButton.AccessibleName = "Play";
        playButton.Padding = S(60, 6, 60, 6);
        body.Controls.Add(playButton);
        playStatus = Note("");
        playStatus.Font = new Font("Segoe UI", 10.5f, FontStyle.Bold);
        playStatus.ForeColor = Snow;
        body.Controls.Add(playStatus);
        AddDiscSetup(body);
        body.Controls.Add(gameNote = Note("The game starts borderless at 1080p in its own 16:9 mode. " +
                               "In the game, F10 opens the settings menu (resolution, aspect, controllers, audio, online play) and F11 switches between fullscreen and a window."));
        // Tips: click the bubble (or the Smash Ball in the sky) for another one.
        var tipRow = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 10, 0, 6) };
        tipRow.Controls.Add(new PictureBox { Image = Sprite("smash_ball", S(44), S(44)), Size = S(44, 44), Margin = S(0, 0, 8, 0) });
        tipLabel = new Label
        {
            AutoSize = true, MaximumSize = new Size(S(620), 0), BackColor = Night, ForeColor = Snow,
            BorderStyle = BorderStyle.FixedSingle, Padding = S(10, 8, 10, 8), Cursor = ballHand, Margin = S(0, 4, 0, 0),
        };
        tipLabel.Click += (_, _) => NextTip();
        tipRow.Controls.Add(tipLabel);
        tipIndex = Random.Shared.Next(Tips.Length) - 1;
        NextTip();
        body.Controls.Add(tipRow);
        var row = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        row.Controls.Add(Btn("Open game folder", (_, _) => Process.Start("explorer.exe", play), Card));
        row.Controls.Add(Btn("Open saves folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(SaveDataDir()).FullName), Card));
        row.Controls.Add(Btn("Import Wii / Dolphin save...", (_, _) => ImportSave(), Card));
        body.Controls.Add(row);
        return card;
    }

    static readonly string[] Tips =
    {
        "Press F10 during the game to change the resolution, aspect ratio or controls without quitting.",
        "F11 flips the game between fullscreen and a window.",
        "Any controller works as a GameCube controller. Remap it on the Controls page.",
        "No controller? Turn on the keyboard on the Controls page and pick your own keys.",
        "Want more fighters? Put REX, Project+Ex or another Wii build on the SD card (Mods page).",
        "Several builds can share the SD card. \"Build to play\" on the Mods page picks the one that runs, or vanilla Brawl.",
        "Play online: F10 in the game, then Online. Host a room or join one; everyone needs the same build.",
        "The first start after adding a build makes the SD card image. Big builds take a few minutes, once.",
        "Click Mario or Sonic up there. Go on.",
        "The fighter under the menu takes a bow when clicked.",
        "Open saves folder takes you straight to your saves. A backup now and then never hurts.",
    };
    Label? tipLabel;
    int tipIndex;

    void NextTip()
    {
        tipIndex = (tipIndex + 1) % Tips.Length;
        if (tipLabel != null) tipLabel.Text = "Tip: " + Tips[tipIndex];
    }

    // A Dolphin save folder, or a data.bin a homebrew save manager exported from a Wii (Brawl's save is no-copy).
    void ImportSave()
    {
        const string saveDir = @"Wii\title\00010000\52534245\data";
        var dolphin = new[] { Environment.SpecialFolder.ApplicationData, Environment.SpecialFolder.MyDocuments }
            .Select(f => Path.Combine(Environment.GetFolderPath(f), "Dolphin Emulator", saveDir)).FirstOrDefault(Directory.Exists);
        using var dialog = new OpenFileDialog
        {
            Title = "Choose a Super Smash Bros. Brawl save: autosv0.bin in a Dolphin save folder, or a data.bin",
            Filter = "Dolphin or Wii save (autosv0.bin, data.bin)|autosv0.bin;data.bin|All files|*.*",
            InitialDirectory = dolphin ?? "",
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        var dataDir = SaveDataDir();
        if (Directory.Exists(dataDir) && Directory.EnumerateFiles(dataDir).Any() &&
            MessageBox.Show(this, "This replaces your current save files. They are kept as a backup next to them (data.backup-<date>).\n\nImport the save?",
                "Import save", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        try
        {
            var files = WiiSave.Import(dialog.FileName, dataDir);
            MessageBox.Show(this, "Imported " + string.Join(", ", files) + ".", "Import save", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
        {
            MessageBox.Show(this, "The save could not be imported:\n" + e.Message, "Import save", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    // `online`: what the game's Online menu does at start (BRAWL_ONLINE: "host:<n>", "host-public:<n>", "browse",
    // "join:<code>", "menu"), or null for a normal start.
    // The usual start: the game straight away (its settings are in its own options menu: F10, or Home on a controller).
    // False when it can't start yet (no disc set up: this window's first-start setup is needed).
    public bool TryStartGame()
    {
        if (!DiscStatus().ok || !File.Exists(gameExe)) return false;
        Gamepads.Release();
        return Process.Start(new ProcessStartInfo(gameExe) { WorkingDirectory = play, UseShellExecute = false }) != null;
    }

    void Launch(string? online = null)
    {
        var (ok, text, _) = DiscStatus();
        if (!ok)
        {
            MessageBox.Show(this, text + "\n\nChoose your disc image with the button under PLAY first.", "Brawl Recompiled", MessageBoxButtons.OK, MessageBoxIcon.Information);
            ShowPage(pages[0].button);
            return;
        }
        if (!File.Exists(gameExe))
        {
            MessageBox.Show(this, "The game files (the engine folder) are missing. Unzip the whole download again, keeping the engine folder next to this launcher.", "Brawl Recompiled", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        Gamepads.Release();
        var start = new ProcessStartInfo(gameExe) { WorkingDirectory = play, UseShellExecute = false };
        if (online != null) start.Environment["BRAWL_ONLINE"] = online;
        var game = Process.Start(start);
        if (game != null)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) => BeginInvoke(Gamepads.Resume);
        }
        else Gamepads.Resume();
        WindowState = FormWindowState.Minimized;
    }

    // ----- Online -----
    Control BuildOnlinePage()
    {
        var (card, body) = NewCard("Online");
        body.Controls.Add(Note("Play Brawl Recompiled with friends over the internet, or on your own network. Host a room and share its code, " +
                               "find a room, or join a friend's code. In the game, F9 opens the same menu at any time.\n" +
                               "Everyone needs the same game: vanilla, or the same mod (Mods page). Each player uses the controller in their own port 1."));
        TextBox Field(string section, string key, string placeholder, int width)
        {
            var box = new TextBox { Width = S(width), Font = new Font("Segoe UI", 11f), BackColor = Night, ForeColor = Snow, BorderStyle = BorderStyle.FixedSingle,
                                    Margin = S(3, 6, 3, 6), PlaceholderText = placeholder, Text = config.Get(section, key) ?? "" };
            box.Leave += (_, _) =>
            {
                var text = box.Text.Trim();
                if (text.Length == 0) config.Remove(section, key); else config.SetString(section, key, text);
                config.Save();
            };
            return box;
        }
        FlowLayoutPanel Labeled(string label, Control control)
        {
            var row = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 2, 0, 2) };
            row.Controls.Add(new Label { Text = label, AutoSize = false, Size = S(200, 34), ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
            row.Controls.Add(control);
            return row;
        }
        body.Controls.Add(Labeled("Your name", Field("online", "name", Environment.MachineName, 260)));

        body.Controls.Add(new Label { Text = "Host a room", Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 14, 0, 2) });
        var players = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(140), FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI", 10.5f), Margin = S(3, 4, 3, 4), BackColor = Night, ForeColor = Snow };
        players.Items.AddRange(new object[] { "2 players", "3 players", "4 players" });
        players.SelectedIndex = 0;
        body.Controls.Add(Labeled("Players", players));
        var listed = new CheckBox { Text = "List it on the online server, so anyone can find it (friends can always join with its code)", Checked = true, AutoSize = true, ForeColor = Snow, Font = new Font("Segoe UI", 10f), Margin = S(3, 4, 3, 4) };
        body.Controls.Add(listed);
        body.Controls.Add(Btn("HOST A ROOM", (_, _) => Launch((listed.Checked ? "host-public:" : "host:") + (players.SelectedIndex + 2))));

        body.Controls.Add(new Label { Text = "Join a room", Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 14, 0, 2) });
        var joinRow = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        joinRow.Controls.Add(Btn("FIND ROOMS", (_, _) => Launch("browse")));
        var code = new TextBox { Width = S(170), Font = new Font("Segoe UI", 12f), BackColor = Night, ForeColor = Snow, BorderStyle = BorderStyle.FixedSingle,
                                 Margin = S(14, 8, 3, 6), PlaceholderText = "Room code", CharacterCasing = CharacterCasing.Upper };
        joinRow.Controls.Add(code);
        joinRow.Controls.Add(Btn("JOIN", (_, _) => { if (code.Text.Trim().Length > 0) Launch("join:" + code.Text.Trim()); }, Card));
        body.Controls.Add(joinRow);
        body.Controls.Add(Note("A room code is 5 letters and digits, shown in the host's Online menu. An IP address or Tailscale name works too."));

        body.Controls.Add(new Label { Text = "Server", Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 14, 0, 2) });
        body.Controls.Add(Labeled("Online server", Field("online", "server", "the built-in server", 320)));
        body.Controls.Add(Note("Leave this empty to use the built-in server. Running your own? Put its address here: an https:// address for a " +
                               "matchmaking Worker (server/worker), or host:port for a relay (server/brawl_online_server.py). Rooms on your network and over Tailscale always work."));
        return card;
    }

    // ----- Display -----
    Control BuildDisplayPage()
    {
        var (card, body) = NewCard("Display");
        body.Controls.Add(Choice("Display mode", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Windowed", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }));
        body.Controls.Add(Choice("Resolution", "resolution_multiplier", new[] { ("1080p", "2.25"), ("4K (2160p, needs a graphics card)", "4.5"), ("Match the screen", "0"), ("1440p", "3"), ("720p", "1.5"), ("480p (original)", "1") }));
        // The game's own 16:9 mode (its widescreen switch) or 4:3; "fill" stretches the 16:9 camera to wider screens.
        body.Controls.Add(Choice("Aspect ratio", "aspect_mode", new[] { ("16:9 (the game's own widescreen)", "1"), ("Fill the screen (ultrawide)", "2"), ("Original 4:3", "0") },
            also: value => config.Set("video", "widescreen", value == "0" ? "false" : "true")));
        body.Controls.Add(Choice("Frame interpolation", "frame_interpolation_fps", new[] { ("120 FPS (on a 120 Hz or faster screen)", "120"), ("Off (60 FPS)", "0"), ("180 FPS (on a 180 Hz or faster screen)", "180"), ("240 FPS (on a 240 Hz or faster screen)", "240") }));
        body.Controls.Add(Choice("Vsync", "vsync", new[] { ("On (no screen tearing)", "true"), ("Off", "false") }));
        body.Controls.Add(Choice("FPS counter", "show_fps", new[] { ("Hidden", "false"), ("Shown", "true") }));
        body.Controls.Add(Note("Changes are saved at once and used the next time the game starts. The same settings are in the game's F10 menu. " +
                               "Frame interpolation inserts frames between the game's 60 fps (it needs a display faster than 60 Hz); turn it off if you see artifacts."));
        return card;
    }

    Control Choice(string label, string key, (string label, string value)[] items, string section = "video", Action<string>? also = null)
    {
        var row = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 4, 0, 4) };
        row.Controls.Add(new Label { Text = label, AutoSize = false, Size = S(200, 32), ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
        var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(300), FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI", 10.5f), Margin = S(3, 4, 3, 4), BackColor = Night, ForeColor = Snow };
        foreach (var item in items) combo.Items.Add(item.label);
        string current = config.Get(section, key) ?? "";
        int index = Array.FindIndex(items, i => i.value.Trim('"') == current || (double.TryParse(i.value, out var a) && double.TryParse(current, out var b) && a == b));
        combo.SelectedIndex = index < 0 ? 0 : index;
        combo.SelectedIndexChanged += (_, _) =>
        {
            var value = items[combo.SelectedIndex].value;
            config.Set(section, key, value);
            also?.Invoke(value);
            config.Save();
        };
        row.Controls.Add(combo);
        return row;
    }

    // ----- Controls -----
    // Every gamepad is a GameCube controller on its port (runtime hle/input/pad.cpp), laid out automatically for its
    // type. The Gamepad view changes one port's buttons through the game's [controller] expr_<port>_<control>
    // expressions (runtime input_bindings.cpp, Dolphin's input names). Only the pads actually connected are listed,
    // refreshed every second. The Keyboard view maps keys to player 1's controller ([gc_keyboard], runtime pad.cpp).
    static readonly (string key, string label)[] GcControls =
    {
        ("buttons_a", "A"), ("buttons_b", "B"), ("buttons_x", "X"), ("buttons_y", "Y"), ("buttons_z", "Z"), ("buttons_start", "Start"),
        ("d_pad_up", "D-pad up"), ("d_pad_down", "D-pad down"), ("d_pad_left", "D-pad left"), ("d_pad_right", "D-pad right"),
        ("triggers_l", "L"), ("triggers_r", "R"),
    };
    // SDL inputs (Gamepads.Active tokens): the name the game's expressions use, and the printed names on Xbox,
    // PlayStation and Nintendo pads.
    static readonly (string token, string expr, string xbox, string ps, string nintendo)[] PadInputs =
    {
        ("a", "Button A", "A", "Cross", "B"), ("b", "Button B", "B", "Circle", "A"), ("x", "Button X", "X", "Square", "Y"), ("y", "Button Y", "Y", "Triangle", "X"),
        ("leftshoulder", "Shoulder L", "LB", "L1", "L"), ("rightshoulder", "Shoulder R", "RB", "R1", "R"),
        ("lefttrigger", "Trigger L", "LT", "L2", "ZL"), ("righttrigger", "Trigger R", "RT", "R2", "ZR"),
        ("back", "Back", "View", "Create", "Minus"), ("start", "Start", "Menu", "Options", "Plus"),
        ("leftstick", "Thumb L", "Left stick click", "L3", "Left stick click"), ("rightstick", "Thumb R", "Right stick click", "R3", "Right stick click"),
        ("dpup", "Pad N", "D-pad up", "D-pad up", "D-pad up"), ("dpdown", "Pad S", "D-pad down", "D-pad down", "D-pad down"),
        ("dpleft", "Pad W", "D-pad left", "D-pad left", "D-pad left"), ("dpright", "Pad E", "D-pad right", "D-pad right", "D-pad right"),
        ("leftx-", "Axis X-", "Left stick left", "Left stick left", "Left stick left"), ("leftx+", "Axis X+", "Left stick right", "Left stick right", "Left stick right"),
        ("lefty-", "Axis Y-", "Left stick up", "Left stick up", "Left stick up"), ("lefty+", "Axis Y+", "Left stick down", "Left stick down", "Left stick down"),
        ("rightx-", "Axis Z-", "Right stick left", "Right stick left", "Right stick left"), ("rightx+", "Axis Z+", "Right stick right", "Right stick right", "Right stick right"),
        ("righty-", "Axis Zr-", "Right stick up", "Right stick up", "Right stick up"), ("righty+", "Axis Zr+", "Right stick down", "Right stick down", "Right stick down"),
    };

    static string PadLabel(int style, string expr)
    {
        var input = PadInputs.FirstOrDefault(i => i.expr == expr);
        if (input.token == null) return expr;
        return style switch { 1 => input.ps, 2 => input.nintendo, _ => input.xbox };
    }

    Control BuildControlsPage()
    {
        var (card, body) = NewCard("Controls");
        var inputRow = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 0, 0, 6) };
        inputRow.Controls.Add(new Label { Text = "Input", AutoSize = false, Size = S(110, 32), ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
        var inputCombo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(460), FlatStyle = FlatStyle.Flat, Margin = S(3, 4, 3, 4), BackColor = Night, ForeColor = Snow };
        inputRow.Controls.Add(inputCombo);
        body.Controls.Add(inputRow);
        var gamepad = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty };
        var keyboard = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Margin = Padding.Empty };
        body.Controls.Add(gamepad);
        body.Controls.Add(keyboard);

        // Gamepad: one button per GameCube control; click it, then press the controller input to bind (Escape or a click cancels).
        int style = 0, port = 1;
        uint padId = 0;
        gamepad.Controls.Add(Note("This controller is a GameCube controller on its port and is set up automatically: the face buttons by position, " +
                                  "the bumpers L and R, the right trigger Z, the left stick to move and the right stick as the C-stick. " +
                                  "To give a control another button as well, click it and press that button on your controller (Escape cancels); " +
                                  "the automatic button keeps working too."));
        var padTable = new TableLayoutPanel { AutoSize = true, ColumnCount = 4, Margin = S(0, 4, 0, 4) };
        gamepad.Controls.Add(padTable);
        Button? capturingPad = null;
        var heldAtStart = new HashSet<string>();
        var padTimer = new System.Windows.Forms.Timer { Interval = 30 };
        string ExprKey(string control) => $"expr_{port}_{control}";
        void FillPadTable()
        {
            padTable.SuspendLayout();
            padTable.Controls.Clear();
            for (int i = 0; i < GcControls.Length; i++)
            {
                var (key, label) = GcControls[i];
                padTable.Controls.Add(new Label { Text = label, AutoSize = true, ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Margin = S(0, 12, 12, 0) }, (i / 6) * 2, i % 6);
                string? custom = config.Get("controller", ExprKey(key));
                var b = Btn(custom == null ? "Automatic" : PadLabel(style, custom), null!, custom == null ? Card : Hover);
                b.AutoSize = false;
                b.Size = S(240, 40);
                b.Margin = S(0, 4, 18, 4);
                b.Tag = key;
                b.Click += (_, _) =>
                {
                    if (capturingPad != null) { EndPadCapture(null); return; }
                    capturingPad = b;
                    b.Text = "Press a button...";
                    b.BackColor = Orange;
                    b.ForeColor = Night;
                    heldAtStart = Gamepads.Active(padId);
                    padTimer.Start();
                };
                padTable.Controls.Add(b, (i / 6) * 2 + 1, i % 6);
            }
            padTable.ResumeLayout();
        }
        void EndPadCapture(string? expr)
        {
            padTimer.Stop();
            if (capturingPad == null) return;
            if (expr != null) { config.SetString("controller", ExprKey((string)capturingPad.Tag!), expr); config.Save(); }
            capturingPad = null;
            FillPadTable();
        }
        padTimer.Tick += (_, _) =>
        {
            var held = Gamepads.Active(padId);
            heldAtStart.IntersectWith(held);  // a key held when the capture began counts once it is released and pressed again
            var pressed = held.FirstOrDefault(t => !heldAtStart.Contains(t) && PadInputs.Any(i => i.token == t));
            if (pressed != null) EndPadCapture(PadInputs.First(i => i.token == pressed).expr);
        };
        cancelPadCapture = () => { if (capturingPad == null) return false; EndPadCapture(null); return true; };
        gamepad.Controls.Add(Btn("Reset to automatic", (_, _) =>
        {
            EndPadCapture(null);
            foreach (var (key, _) in GcControls) config.Remove("controller", ExprKey(key));
            config.Save();
            FillPadTable();
        }, Card));

        // Keyboard: player 1's GameCube controller (Config.toml [gc_keyboard], applied by the runtime's pad start).
        var useKeyboard = new CheckBox
        {
            Text = "Use the keyboard and mouse as player 1's controller", AutoSize = true, ForeColor = Snow,
            Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Margin = S(0, 4, 0, 4), Cursor = ballHand,
            Checked = config.Get("gc_keyboard", "enabled") == "true",
        };
        keyboard.Controls.Add(useKeyboard);
        keyboard.Controls.Add(Note("Click a control, then press the key for it, or click the control again with the mouse button you want " +
                                   "(Escape cancels, Delete clears). It replaces the gamepad on port 1; the game's F10 menu has the same settings."));
        var keyTable = new TableLayoutPanel { AutoSize = true, ColumnCount = 4, Margin = S(0, 4, 0, 4) };
        var keyButtons = new List<Button>();
        for (int i = 0; i < KeyboardControls.Length; i++)
        {
            var (key, label, defaults) = KeyboardControls[i];
            keyTable.Controls.Add(new Label { Text = label, AutoSize = true, ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), Margin = S(0, 12, 12, 0) }, (i / 10) * 2, i % 10);
            var b = Btn(KeyText(config.Get("gc_keyboard", key) ?? defaults), null!, Card);
            b.AutoSize = false;
            b.Size = S(200, 38);
            b.Margin = S(0, 3, 24, 3);
            b.Tag = key;
            b.Click += (s, _) => { if (capturing == null) BeginCapture((Button)s!); };
            b.MouseDown += (s, e) =>
            {
                // A mouse button pressed on the control being set binds that mouse button.
                if (capturing != s || e.Button == MouseButtons.Left && captureStarted.ElapsedMilliseconds < 300) return;
                EndCapture(e.Button switch { MouseButtons.Left => "Mouse Left", MouseButtons.Right => "Mouse Right", MouseButtons.Middle => "Mouse Middle", _ => null });
            };
            keyButtons.Add(b);
            keyTable.Controls.Add(b, (i / 10) * 2 + 1, i % 10);
        }
        keyboard.Controls.Add(keyTable);
        void SaveAllKeys()
        {
            // The whole layout goes to Config.toml once the keyboard is turned on, so the game applies exactly what is shown.
            foreach (var (key, _, defaults) in KeyboardControls)
                if (config.Get("gc_keyboard", key) == null) config.SetString("gc_keyboard", key, defaults);
        }
        useKeyboard.CheckedChanged += (_, _) =>
        {
            config.Set("gc_keyboard", "enabled", useKeyboard.Checked ? "true" : "false");
            if (useKeyboard.Checked) SaveAllKeys();
            config.Save();
        };
        keyboard.Controls.Add(Btn("Reset to the WASD + mouse layout", (_, _) =>
        {
            foreach (var (key, _, defaults) in KeyboardControls) config.SetString("gc_keyboard", key, defaults);
            config.Save();
            foreach (var b in keyButtons) b.Text = KeyText(KeyboardControls.First(c => c.key == (string)b.Tag!).defaults);
        }, Card));

        // The input list: connected pads first (in port order), then the keyboard.
        var shown = new List<(uint id, string name, int style)>();
        void ShowSelected()
        {
            int i = inputCombo.SelectedIndex;
            bool isPad = i >= 0 && i < shown.Count;
            gamepad.Visible = isPad;
            keyboard.Visible = !isPad;
            if (isPad) { EndPadCapture(null); style = shown[i].style; padId = shown[i].id; port = i + 1; FillPadTable(); }
        }
        void RefreshInputs()
        {
            var pads = Gamepads.Connected();
            if (pads.Count == 0 && screenshotDir != null) pads.Add((0, "Test controller (screenshot mode)", 0));
            if (pads.SequenceEqual(shown) && inputCombo.Items.Count > 0) return;
            uint? selectedId = inputCombo.SelectedIndex >= 0 && inputCombo.SelectedIndex < shown.Count ? shown[inputCombo.SelectedIndex].id : null;
            bool keyboardSelected = inputCombo.Items.Count > 0 && inputCombo.SelectedIndex == shown.Count;
            shown.Clear();
            shown.AddRange(pads);
            inputCombo.Items.Clear();
            for (int i = 0; i < pads.Count; i++) inputCombo.Items.Add($"Port {i + 1}: {pads[i].name}  ({new[] { "Xbox", "PlayStation", "Nintendo" }[pads[i].style]} layout)");
            inputCombo.Items.Add(pads.Count == 0 ? "Keyboard  (no controller connected - plug one in and it appears here)" : "Keyboard");
            int keep = selectedId is uint id ? shown.FindIndex(p => p.id == id) : -1;
            inputCombo.SelectedIndex = keyboardSelected ? shown.Count : keep >= 0 ? keep : 0;
            ShowSelected();
        }
        inputCombo.SelectedIndexChanged += (_, _) => ShowSelected();
        RefreshInputs();
        var poll = new System.Windows.Forms.Timer { Interval = 1000 };
        poll.Tick += (_, _) => { if (card.Visible) RefreshInputs(); };
        poll.Start();
        return card;
    }

    // The keyboard's GameCube controls (runtime pad.cpp ApplyKeyboardConfig reads the same keys), with the in-game
    // menu's WASD + mouse preset as defaults.
    static readonly (string key, string label, string defaults)[] KeyboardControls =
    {
        ("a", "A", "Mouse Left"), ("b", "B", "Space"), ("x", "X", "E"), ("y", "Y", "Q"), ("z", "Z", "Mouse Middle"),
        ("l", "L (shield)", "Left Shift"), ("r", "R (shield)", "Mouse Right"), ("start", "Start", "Return"),
        ("dpad_up", "D-pad up", "Up"), ("dpad_down", "D-pad down", "Down"), ("dpad_left", "D-pad left", "Left"), ("dpad_right", "D-pad right", "Right"),
        ("stick_up", "Stick up", "W"), ("stick_down", "Stick down", "S"), ("stick_left", "Stick left", "A"), ("stick_right", "Stick right", "D"),
        ("cstick_up", "C-stick up", "I"), ("cstick_down", "C-stick down", "K"), ("cstick_left", "C-stick left", "J"), ("cstick_right", "C-stick right", "L"),
    };

    Button? capturing;
    string? captureKey;
    readonly Stopwatch captureStarted = new();

    static string KeyText(string key) => key.Length == 0 ? "None" : key;

    void BeginCapture(Button button)
    {
        capturing = button;
        captureKey = button.Text;
        captureStarted.Restart();
        button.Text = "Press a key...";
        button.BackColor = Orange;
        button.ForeColor = Night;
    }

    // name: the binding (an SDL key name or "Mouse ..."), "" to clear it, null to cancel.
    void EndCapture(string? name)
    {
        if (capturing == null) return;
        if (name != null)
        {
            config.SetString("gc_keyboard", (string)capturing.Tag!, name);
            config.Save();
            captureKey = KeyText(name);
        }
        capturing.Text = captureKey!;
        capturing.BackColor = Card;
        capturing.ForeColor = Snow;
        capturing = null;
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (capturing != null)
        {
            e.SuppressKeyPress = true;
            e.Handled = true;
            if (e.KeyCode == Keys.Escape) EndCapture(null);
            else if (e.KeyCode == Keys.Delete) EndCapture("");
            else if (SdlName(e.KeyCode) is { } name) EndCapture(name);
            return;  // a key SDL has no name for here: keep waiting
        }
        if (e.KeyCode == Keys.Escape && cancelPadCapture?.Invoke() == true) { e.Handled = true; return; }
        base.OnKeyDown(e);
    }

    // Arrow keys and Space normally move focus or press the focused button: while a control is being set they are keys.
    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if (capturing != null)
        {
            OnKeyDown(new KeyEventArgs(keyData));
            return true;
        }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    // SDL scancode names, as the runtime reads them (SDL_GetScancodeFromName).
    static string? SdlName(Keys k) => (k & Keys.KeyCode) switch
    {
        >= Keys.A and <= Keys.Z => (k & Keys.KeyCode).ToString(),
        >= Keys.D0 and <= Keys.D9 => ((char)('0' + ((k & Keys.KeyCode) - Keys.D0))).ToString(),
        >= Keys.NumPad0 and <= Keys.NumPad9 => "Keypad " + ((k & Keys.KeyCode) - Keys.NumPad0),
        >= Keys.F1 and <= Keys.F9 => (k & Keys.KeyCode).ToString(),
        Keys.Up => "Up", Keys.Down => "Down", Keys.Left => "Left", Keys.Right => "Right",
        Keys.Space => "Space", Keys.Return => "Return", Keys.Tab => "Tab", Keys.Back => "Backspace", Keys.Insert => "Insert",
        Keys.Home => "Home", Keys.End => "End", Keys.PageUp => "PageUp", Keys.PageDown => "PageDown", Keys.CapsLock => "CapsLock",
        Keys.ShiftKey or Keys.LShiftKey => "Left Shift", Keys.RShiftKey => "Right Shift",
        Keys.ControlKey or Keys.LControlKey => "Left Ctrl", Keys.RControlKey => "Right Ctrl", Keys.Menu or Keys.LMenu => "Left Alt",
        Keys.OemMinus => "-", Keys.Oemplus => "=", Keys.OemOpenBrackets => "[", Keys.OemCloseBrackets => "]", Keys.Oemcomma => ",",
        Keys.OemPeriod => ".", Keys.OemQuestion => "/", Keys.OemSemicolon => ";", Keys.OemQuotes => "'", Keys.Oemtilde => "`",
        _ => null,
    };

    // ----- Game disc (part of the Play page) -----
    // One button sets the disc up (or changes it later); extraction progress shows under it, and the dump
    // instructions only while no disc is set up.
    void AddDiscSetup(FlowLayoutPanel body)
    {
        var progress = new ProgressBar { Size = S(640, 22), Margin = S(0, 6, 0, 2), Visible = false };
        var progressText = Note("");
        progressText.Visible = false;
        progressText.TextChanged += (_, _) => progressText.Visible = progressText.Text.Length > 0;  // no empty gap
        var choose = chooseDisc = Btn("Choose disc image (.iso, .ciso, .wbfs)...", null!);
        choose.AutoSizeMode = AutoSizeMode.GrowAndShrink;  // "Change disc..." is shorter
        choose.Click += async (_, _) =>
        {
            using var dialog = new OpenFileDialog { Title = "Choose your Super Smash Bros. Brawl disc image", Filter = "Wii disc images (*.iso;*.ciso;*.wbfs)|*.iso;*.ciso;*.wbfs|All files|*.*" };
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            WiiDisc disc;
            bool extracted = false;
            try { disc = WiiDisc.Open(dialog.FileName); }
            catch (Exception ex) { progressText.Text = "Could not read that image: " + ex.Message; return; }
            using (disc)
            {
                if (disc.Problem() is { Length: > 0 } problem) { progressText.Text = problem; return; }
                choose.Enabled = false;
                progress.Visible = true;
                var staging = discDir + ".new";
                try
                {
                    if (Directory.Exists(staging)) Directory.Delete(staging, true);
                    var report = new Progress<(int done, int total, string name)>(p =>
                    {
                        progress.Maximum = p.total;
                        progress.Value = Math.Min(p.done, p.total);
                        progressText.Text = $"Extracting {p.done}/{p.total}  {p.name}";
                    });
                    await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                    if (disc.Holes > 0) throw new InvalidDataException("the image is incomplete (blocks missing); dump the disc again.");
                    if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                    Directory.Move(staging, discDir);
                    config.SetString("paths", "dvd_root", "../../Disc/DATA");  // relative to <game folder>\UserData (one level below the root in both layouts), so the folder can move
                    config.Save();
                    progressText.Text = "Done. The disc is extracted and ready.";
                    extracted = true;
                }
                catch (Exception ex)
                {
                    progressText.Text = "Extraction failed: " + ex.Message;
                }
                finally
                {
                    choose.Enabled = true;
                    progress.Visible = false;
                    RefreshStatus();
                }
            }
            // First start: the disc is ready, so on to the game.
            if (extracted && !selfTest && TryStartGame()) Close();
        };
        body.Controls.Add(choose);
        // #20: a disc already extracted (by Dolphin, or an earlier install) is used where it is, no image needed.
        body.Controls.Add(Btn("Use an extracted game folder...", (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { Description = "Choose the extracted Super Smash Bros. Brawl folder (the one holding sys and files)" };
            if (dialog.ShowDialog(this) != DialogResult.OK) return;
            var dir = dialog.SelectedPath;
            if (!File.Exists(Path.Combine(dir, "sys", "boot.bin")) && File.Exists(Path.Combine(dir, "DATA", "sys", "boot.bin"))) dir = Path.Combine(dir, "DATA");
            string? before = config.Get("paths", "dvd_root");
            config.SetString("paths", "dvd_root", dir);
            var (ok, text, _) = DiscStatus();
            if (!ok)
            {
                if (before == null) config.Remove("paths", "dvd_root"); else config.SetString("paths", "dvd_root", before);
                progressText.Text = text;
            }
            config.Save();
            RefreshStatus();
            if (ok && !selfTest && TryStartGame()) Close();
        }, Card));
        body.Controls.Add(progress);
        body.Controls.Add(progressText);
        body.Controls.Add(dumpHelp = Note(
            "This port runs your own copy of Super Smash Bros. Brawl (USA, RSBE01). It needs a disc image of the disc you own, " +
            "which the launcher decrypts and extracts once into its Disc folder (about 7 GB; the disc is dual-layer, so give it a few minutes).\n\n" +
            "Making a legal image of your disc (Wii or Wii U consoles can read the disc; PC drives can't):\n" +
            "1. On a Wii with the Homebrew Channel, run CleanRip (or USB Loader GX's install option).\n" +
            "2. Insert Super Smash Bros. Brawl and dump it to an SD card or USB drive. CleanRip writes a .iso " +
            "(it may be split into .part files: join them with  copy /b  on the PC); USB Loader GX writes a .wbfs.\n" +
            "3. Copy the file to this PC and choose it with the button above. .iso, .ciso and .wbfs all work.\n" +
            "The USA release, revision 1, works (the European and Japanese discs do not). " +
            "A Dolphin-extracted folder also works: put it in Disc\\DATA next to the launcher."));
    }

    (bool ok, string text, string path) DiscStatus()
    {
        string? configured = config.Get("paths", "dvd_root");
        string path = configured == null ? discDir : Path.GetFullPath(Path.Combine(userData, configured));
        if (!File.Exists(Path.Combine(path, "sys", "boot.bin")))
            return (false, "No game disc set up yet. Choose your Super Smash Bros. Brawl disc image below; the launcher extracts it into its Disc folder.", path);
        var header = new byte[8];
        using (var f = File.OpenRead(Path.Combine(path, "sys", "boot.bin"))) f.ReadExactly(header, 0, 8);
        string id = System.Text.Encoding.ASCII.GetString(header, 0, 6);
        if (id != WiiDisc.Game || header[7] != WiiDisc.GameRevision)
            return (false, $"The disc folder holds {id} revision {header[7]}, not Super Smash Bros. Brawl USA revision {WiiDisc.GameRevision}: {path}", path);
        return (true, "Super Smash Bros. Brawl (USA) is ready: " + path, path);
    }

    void RefreshStatus()
    {
        var (ok, text, _) = DiscStatus();
        playStatus.Text = ok ? "Disc: ready.  SD card: " + SdNames() + ".  Mods: " + ModNames() : text;
        playStatus.ForeColor = ok ? Snow : Orange;
        chooseDisc.Text = ok ? "Change disc..." : "Choose disc image (.iso, .ciso, .wbfs)...";
        chooseDisc.BackColor = ok ? Card : Orange;
        chooseDisc.ForeColor = ok ? Snow : Night;
        dumpHelp.Visible = !ok;
        gameNote.Visible = ok;
        if (sdStatus != null) sdStatus.Text = "On the card now: " + SdNames() + ".";
    }

    string ModNames()
    {
        var names = Directory.Exists(modsDir) ? Directory.GetDirectories(modsDir).Where(d => !File.Exists(Path.Combine(d, "disabled"))).Select(Path.GetFileName).ToArray() : Array.Empty<string>();
        return names.Length == 0 ? "none" : string.Join(", ", names);
    }

    // The builds on the card: its folders holding Gecko codes (RSBE01.GCT), the ones the game can load codes from.
    List<string> SdBuilds() => Directory.Exists(sdDir)
        ? Directory.GetDirectories(sdDir).Where(d => Directory.EnumerateFiles(d).Any(f => Path.GetFileName(f).Equals("RSBE01.GCT", StringComparison.OrdinalIgnoreCase)))
            .Select(d => Path.GetFileName(d)!).OrderBy(n => n, StringComparer.Ordinal).ToList()
        : new List<string>();

    // The folders on the virtual SD card (a mod's SD card contents, Project+'s "Project+", "private" and "apps").
    string SdNames()
    {
        var names = Directory.Exists(sdDir) ? Directory.GetDirectories(sdDir).Select(Path.GetFileName).ToArray() : Array.Empty<string>();
        return names.Length == 0 ? "empty" : string.Join(", ", names);
    }

    // ----- Mods -----
    Control BuildModsPage()
    {
        var (card, body) = NewCard("Mods");
        body.Controls.Add(new Label { Text = "SD card mods (REX, Project+Ex, Project+, Brawl Minus, Legacy XP)", Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 4, 0, 2) });
        body.Controls.Add(Note("The SD card folder is the game's SD card, the way a Wii sees one. Add a mod with the buttons below (choose its Wii / console download: the .zip, or the folder you extracted it to), or copy a build's card folder from that download " +
                               "download into it: rex_ for REX (the biggest roster), P+Ex, Project+, Minusery (Brawl Minus) or LegacyXP. Its Gecko codes " +
                               "(RSBE01.GCT and gc.txt) load with it, and the game's own SD features (snapshots, replays, custom stages) use the card too. " +
                               "The card image is rebuilt when the folder changes; a big build takes a few minutes the first time."));
        sdStatus = Note("");
        sdStatus.ForeColor = Snow;
        body.Controls.Add(sdStatus);
        body.Controls.Add(Btn("Open SD card folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(sdDir).FullName)));
        // Which build's Gecko codes load when several share the card (Config.toml [mods] sd_codes = the folder).
        var codesRow = new FlowLayoutPanel { AutoSize = true, Margin = S(0, 4, 0, 4) };
        codesRow.Controls.Add(new Label { Text = "Build to play", AutoSize = false, Size = S(200, 32), ForeColor = Snow, Font = new Font("Segoe UI", 10.5f, FontStyle.Bold), TextAlign = ContentAlignment.MiddleLeft });
        var codesCombo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = S(300), FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI", 10.5f), Margin = S(3, 4, 3, 4), BackColor = Night, ForeColor = Snow };
        codesRow.Controls.Add(codesCombo);
        body.Controls.Add(codesRow);
        bool fillingCodes = false;
        void FillCodes()
        {
            fillingCodes = true;
            codesCombo.Items.Clear();
            codesCombo.Items.Add("The first build on the card");
            codesCombo.Items.Add("Vanilla Brawl (no mods)");
            var builds = SdBuilds();
            foreach (var b in builds) codesCombo.Items.Add(b);
            var chosen = config.Get("mods", "sd_codes") ?? "";
            int i = builds.IndexOf(chosen);
            codesCombo.SelectedIndex = chosen == "vanilla" ? 1 : i < 0 ? 0 : i + 2;
            fillingCodes = false;
        }
        codesCombo.SelectedIndexChanged += (_, _) =>
        {
            if (fillingCodes) return;
            if (codesCombo.SelectedIndex <= 0) config.Remove("mods", "sd_codes");
            else if (codesCombo.SelectedIndex == 1) config.SetString("mods", "sd_codes", "vanilla");  // no codes, no SD card
            else config.SetString("mods", "sd_codes", (string)codesCombo.Items[codesCombo.SelectedIndex]!);
            config.Save();
        };
        FillCodes();
        // Add a mod: the build folder is found inside a .zip or an extracted download and copied onto the card.
        void AddMod(string source)
        {
            sdStatus.Text = "Adding the mod...";
            Task.Run(() =>
            {
                try
                {
                    var name = ModInstall.Install(source, Directory.CreateDirectory(sdDir).FullName,
                        n => { if (n % 200 == 0) BeginInvoke(() => sdStatus.Text = $"Adding the mod... {n} files"); });
                    BeginInvoke(() =>
                    {
                        config.SetString("mods", "sd_codes", name);
                        config.Save();
                        FillCodes();
                        RefreshStatus();
                        sdStatus.Text = $"Added {name}; it is now the build to play. " + sdStatus.Text;
                    });
                }
                catch (Exception ex)
                {
                    BeginInvoke(() => sdStatus.Text = ex.Message);
                }
            });
        }
        var addRow = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        addRow.Controls.Add(Btn("Add a mod from its .zip...", (_, _) =>
        {
            using var dialog = new OpenFileDialog { Title = "Choose the mod's Wii download", Filter = "Mod downloads (*.zip)|*.zip|All files|*.*" };
            if (dialog.ShowDialog(this) == DialogResult.OK) AddMod(dialog.FileName);
        }));
        addRow.Controls.Add(Btn("Add a mod from a folder...", (_, _) =>
        {
            using var dialog = new FolderBrowserDialog { Description = "Choose the folder the mod's Wii download was extracted to" };
            if (dialog.ShowDialog(this) == DialogResult.OK) AddMod(dialog.SelectedPath);
        }, Card));
        body.Controls.Add(addRow);
        body.Controls.Add(new Label { Text = "File and texture mods", Font = new Font("Segoe UI", 12f, FontStyle.Bold), ForeColor = Snow, AutoSize = true, Margin = S(0, 14, 0, 2) });
        body.Controls.Add(Note("How to install one:\n" +
                               "1. Click Open mods folder.\n" +
                               "2. Put the mod's folder in it (one folder per mod: files\\ mirrors the disc, textures\\ holds texture replacements).\n" +
                               "3. Click Refresh. Ticked mods load when the game starts; untick one to turn it off."));
        var row = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
        row.Controls.Add(Btn("Open mods folder", (_, _) => Process.Start("explorer.exe", Directory.CreateDirectory(modsDir).FullName)));
        row.Controls.Add(Btn("Refresh", (_, _) => { LoadMods(); FillCodes(); RefreshStatus(); }, Card));
        body.Controls.Add(row);
        modList = new ListView { View = View.Details, CheckBoxes = true, Size = S(660, 200), FullRowSelect = true, Font = new Font("Segoe UI", 10.5f), Margin = S(0, 6, 0, 6), HeaderStyle = ColumnHeaderStyle.Nonclickable, BackColor = Night, ForeColor = Snow, BorderStyle = BorderStyle.FixedSingle };
        modList.Columns.Add("Installed mods", S(630));
        modList.ItemCheck += (_, e) =>
        {
            var dir = (string)modList.Items[e.Index].Tag!;
            var marker = Path.Combine(dir, "disabled");
            if (e.NewValue == CheckState.Checked) File.Delete(marker); else File.WriteAllText(marker, "");
            BeginInvoke(RefreshStatus);
        };
        body.Controls.Add(modList);
        LoadMods();
        return card;
    }

    void LoadMods()
    {
        modList.Items.Clear();
        if (!Directory.Exists(modsDir)) return;
        foreach (var dir in Directory.GetDirectories(modsDir).OrderBy(d => d, StringComparer.Ordinal))
            modList.Items.Add(new ListViewItem(Path.GetFileName(dir)) { Tag = dir, Checked = !File.Exists(Path.Combine(dir, "disabled")) });
    }

    sealed class BufferedFlow : FlowLayoutPanel
    {
        public BufferedFlow() { DoubleBuffered = true; ResizeRedraw = true; }
    }

    sealed class DoubleBufferedPanel : Panel
    {
        public DoubleBufferedPanel() { DoubleBuffered = true; ResizeRedraw = true; }
    }
}
