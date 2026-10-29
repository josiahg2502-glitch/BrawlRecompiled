using System.Diagnostics;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace BrawlRecompiled;

// The Linux launcher window: the Windows launcher (launcher\MainForm.cs) page for page - a dark header, a nav column
// (Play, Display, Controls, Mods) and one card per page. Everything it changes goes to engine/UserData/Config.toml,
// read by the game at start. Keep the two in step when either changes.
sealed class MainWindow : Window
{
    public static readonly Color Night = Color.Parse("#1A1E2C"), Card = Color.Parse("#262B3C"), Snow = Color.Parse("#F2F2F6"),
        Dim = Color.Parse("#B8BCCB"), Orange = Color.Parse("#FF8C28"), Red = Color.Parse("#D83A3A"), Edge = Color.Parse("#3C4358"),
        Hover = Color.Parse("#343A50"), Gold = Color.Parse("#FFC43A");
    public static readonly IBrush SnowBrush = new SolidColorBrush(Snow), EdgeBrush = new SolidColorBrush(Edge);
    static readonly FontFamily Font = new("fonts:Inter#Inter");
    // WinForms sizes are in points; these are the same sizes in pixels (10.5pt, 13pt, 18pt, 24pt).
    const double Body = 14, NavSize = 17.3, Heading = 24, PlaySize = 32;

    readonly bool selfTest;
    readonly string? screenshotDir;
    // play = the folder holding the game programs and their UserData: engine/ in a release.
    readonly string root, play, gameExe, userData, configPath, modsDir, sdDir, discDir;
    static readonly string AppVersion = typeof(MainWindow).Assembly.GetName().Version is { } v ? $"{v.Major}.{v.Minor}.{v.Build}" : "?";
    readonly GameConfig config;
    readonly Panel content = new() { Margin = new Thickness(16, 12, 20, 16) };
    readonly List<(PaperButton button, Control page)> pages = new();
    readonly StackPanel navButtons = new() { Margin = new Thickness(16, 18, 0, 0), VerticalAlignment = VerticalAlignment.Top, HorizontalAlignment = HorizontalAlignment.Left };
    TextBlock playStatus = null!, dumpHelp = null!, gameNote = null!, sdStatus = null!;
    StackPanel modList = null!;
    PaperButton chooseDisc = null!, playButton = null!;
    Func<bool>? cancelPadCapture;  // the Controls page's gamepad capture: Escape ends it

    public MainWindow(bool selfTest, string? screenshotDir = null)
    {
        this.selfTest = selfTest;
        this.screenshotDir = screenshotDir;
        (root, play, gameExe) = FindGame();
        userData = Path.Combine(play, "UserData");
        configPath = Path.Combine(userData, "Config.toml");
        modsDir = Path.Combine(userData, "mods");
        sdDir = Path.Combine(userData, "SD");
        discDir = Path.Combine(root, "Disc", "DATA");
        config = new GameConfig(configPath);
        EnsureDefaults();
        Gamepads.Init(play);

        Title = "Brawl Recompiled  v" + AppVersion;
        BallCursor = MakeBallCursor(clickable: false);
        BallHand = MakeBallCursor(clickable: true);
        Cursor = BallCursor;
        using (var icon = typeof(MainWindow).Assembly.GetManifestResourceStream("smash.ico")) if (icon != null) Icon = new WindowIcon(icon);
        Width = 1100;
        Height = 760;
        if (Screens.Primary is { } screen)
        {
            var area = screen.WorkingArea.Size.ToSize(screen.Scaling);
            Width = Math.Min(1100, area.Width - 40);
            Height = Math.Min(760, area.Height - 60);
        }
        MinWidth = Math.Min(940, Width);
        MinHeight = Math.Min(640, Height);
        Background = new SolidColorBrush(Night);
        FontFamily = Font;
        FontSize = Body;
        Foreground = SnowBrush;
        WindowStartupLocation = WindowStartupLocation.CenterScreen;

        var header = new HeaderView(this) { Height = 150 };
        var fighter = new NavFighterView(this);
        var grid = new Grid { RowDefinitions = new RowDefinitions("150,*"), ColumnDefinitions = new ColumnDefinitions("220,*") };
        Grid.SetColumnSpan(header, 2);
        Grid.SetRow(fighter, 1);
        Grid.SetRow(navButtons, 1);
        Grid.SetRow(content, 1);
        Grid.SetColumn(content, 1);
        grid.Children.Add(header);
        grid.Children.Add(fighter);  // under the menu buttons, which sit on top of it
        grid.Children.Add(navButtons);
        grid.Children.Add(content);
        Content = grid;

        var frame = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        frame.Tick += (_, _) =>
        {
            if (WindowState == WindowState.Minimized) return;
            header.InvalidateVisual();
            fighter.InvalidateVisual();
            float glow = (float)(Math.Sin(T * 3) + 1) / 2;  // PLAY breathes between two reds
            playButton.Back = Color.FromRgb((byte)Lerp(Red.R, 255, glow), (byte)Lerp(Red.G, 100, glow), (byte)Lerp(Red.B, 70, glow));
        };
        frame.Start();

        AddPage("Play", BuildPlayPage());
        AddPage("Display", BuildDisplayPage());
        AddPage("Controls", BuildControlsPage());
        AddPage("Mods", BuildModsPage());
        AddPage("Online", BuildOnlinePage());
        ShowPage(pages[0].button);
        AddHandler(KeyDownEvent, OnKey, RoutingStrategies.Tunnel);
        Opened += async (_, _) =>
        {
            if (screenshotDir != null)
            {
                // Headless visual check: every page drawn to <dir>/<page>.png.
                Directory.CreateDirectory(screenshotDir);
                foreach (var p in pages)
                {
                    ShowPage(p.button);
                    await Task.Delay(400);
                    var size = new PixelSize((int)Bounds.Width, (int)Bounds.Height);
                    using var bmp = new RenderTargetBitmap(size);
                    bmp.Render(this);
                    bmp.Save(Path.Combine(screenshotDir, p.button.Text.Replace(' ', '_') + ".png"));
                }
                Close();
            }
            else if (selfTest)
            {
                foreach (var p in pages) ShowPage(p.button);
                int code = !File.Exists(gameExe) ? 6 : !Gamepads.Ready ? 4 : Gamepads.Connected().All(p => Gamepads.Opens(p.id)) ? 0 : 5;
                // posted: the lifetime cannot shut down while it is still starting
                Dispatcher.UIThread.Post(() => (Application.Current?.ApplicationLifetime as IClassicDesktopStyleApplicationLifetime)?.Shutdown(code));
            }
        };
    }

    // The release keeps the game in engine/ next to this launcher; search up from the launcher's own folder.
    static (string root, string dir, string exe) FindGame()
    {
        var start = new DirectoryInfo(AppContext.BaseDirectory);
        for (var d = start; d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "engine", "brawl-engine")))
                return (d.FullName, Path.Combine(d.FullName, "engine"), Path.Combine(d.FullName, "engine", "brawl-engine"));
        return (start.FullName, Path.Combine(start.FullName, "engine"), Path.Combine(start.FullName, "engine", "brawl-engine"));
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

    void AddPage(string title, Control page)
    {
        var button = new PaperButton(title, Card, NavSize)
        {
            Width = 188, Height = 50, Padding = new Thickness(12, 0, 0, 0), Margin = new Thickness(0, 0, 0, 10), HoverColor = Hover,
        };
        button.Click += () => ShowPage(button);
        button.ShowImage(Art($"text_nav_{title.ToLowerInvariant()}"));  // the title in the game's font
        page.IsVisible = false;
        content.Children.Add(page);
        navButtons.Children.Add(button);
        pages.Add((button, page));
    }

    void ShowPage(PaperButton button)
    {
        foreach (var (b, page) in pages)
        {
            bool on = b == button;
            b.Back = on ? Orange : Card;
            b.TextBrush = on ? new SolidColorBrush(Night) : SnowBrush;
            b.ShowImage(Art($"text_nav_{b.Text.ToLowerInvariant()}{(on ? "_on" : "")}"));
            page.IsVisible = on;
        }
        RefreshStatus();
    }

    // ----- The header: the title over a dark sky with drifting embers -----
    readonly Stopwatch clock = Stopwatch.StartNew();
    double T => clock.Elapsed.TotalSeconds;

    static double Lerp(double a, double b, float f) => a + (b - a) * f;

    sealed class HeaderView : Control
    {
        readonly MainWindow w;
        public HeaderView(MainWindow window) { w = window; ClipToBounds = true; }

        public override void Render(DrawingContext g)
        {
            var r = new Rect(Bounds.Size);
            if (r.Width <= 0 || r.Height <= 0) return;
            double t = w.T;
            g.FillRectangle(new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative), EndPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
                GradientStops = { new GradientStop(Color.Parse("#0E1120"), 0), new GradientStop(Card, 1) },
            }, r);
            // Embers rise and fade, each on its own lane.
            for (int i = 0; i < 24; i++)
            {
                double life = (t * (0.12 + 0.04 * (i % 5)) + i * 0.37) % 1.0;
                double x = (i * 97 % 100) / 100.0 * r.Width + Math.Sin(t * 1.3 + i) * 8;
                double y = r.Height - life * r.Height;
                double size = 3 + i % 3;
                g.DrawEllipse(new SolidColorBrush(i % 2 == 0 ? Orange : Gold, 0.67 * (1 - life)), null, new Rect(x, y, size, size));
            }
            g.DrawLine(new Pen(new SolidColorBrush(Orange), 4), new Point(0, r.Height - 2), new Point(r.Width, r.Height - 2));
            // The title in the game's own title font, shrunk if the window is narrow so it stays clear of the fighters.
            double titleBottom = 22 + 50;
            if (Art("text_title") is { } title)
            {
                double h = Math.Min(title.PixelSize.Height / 2.0, (r.Width - 2 * 200) / 9.0 * title.PixelSize.Height / 116.0);
                double tw = title.PixelSize.Width * h / title.PixelSize.Height;
                g.DrawImage(title, new Rect(r.Width / 2 - tw / 2, 14, tw, h));
                titleBottom = 14 + h;
            }
            var sub = new FormattedText("Super Smash Bros. Brawl (Wii) on PC  -  made by JosiahG  -  v" + AppVersion, CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight, new Typeface(Font, FontStyle.Normal, FontWeight.Bold), Body, SnowBrush);
            var tag = new Rect(r.Width / 2 - sub.Width / 2 - 10, titleBottom + 2, sub.Width + 20, sub.Height + 4);
            g.DrawRectangle(new SolidColorBrush(Card), new Pen(EdgeBrush, 2), tag, 8, 8);
            g.DrawText(sub, new Point(r.Width / 2 - sub.Width / 2, tag.Y + 2));

            // The Smash Ball drifts across the sky, turning, trailing sparkles; a click on it shows another tip.
            double half = r.Width / 2.0 - 230;
            Point At(double when) => new(r.Width / 2.0 + Math.Sin(when * 0.42) * half, 42 + Math.Sin(when * 1.2) * 18);
            for (int k = 7; k >= 1; k--)
            {
                var p = At(t - k * 0.08);
                Sparkle(g, p.X + Math.Sin(t * 7 + k) * 4, p.Y + Math.Cos(t * 5 + k) * 4, 12 - k, k % 2 == 0 ? Color.FromArgb((byte)(230 - k * 28), Gold.R, Gold.G, Gold.B)
                                                                                                         : Color.FromArgb((byte)(230 - k * 28), 255, 255, 255));
            }
            var bp = At(t);
            const double ball = 46;
            BallRect = new Rect(bp.X - ball / 2, bp.Y - ball / 2, ball, ball);
            if (Art("smash_ball") is { } ballImage)
                using (g.PushTransform(Matrix.CreateTranslation(-bp.X, -bp.Y) * Matrix.CreateRotation(Math.Sin(t * 0.9) * 0.44) * Matrix.CreateTranslation(bp.X, bp.Y)))
                    g.DrawImage(ballImage, BallRect);

            // Mario and Sonic bounce at the sides; a click makes one hop.
            double Lift(string name, double phase)
            {
                double hop = w.Hop(name, 0.7);
                return Math.Abs(Math.Sin(t * 2.2 + phase)) * 5 + (hop < 0 ? 0 : Math.Sin(hop * Math.PI) * 30);
            }
            const double fw = 104, fh = 130;
            LeftRect = new Rect(18, r.Height - fh + 10 - Lift("left", 0), fw, fh);
            RightRect = new Rect(r.Width - fw - 18, r.Height - fh + 10 - Lift("right", 1.3), fw, fh);
            using (g.PushClip(new Rect(0, 0, r.Width, r.Height - 4)))
            {
                if (Art("fighter_mario") is { } mario) g.DrawImage(mario, LeftRect);
                if (Art("fighter_sonic") is { } sonic)
                    using (g.PushTransform(Matrix.CreateScale(-1, 1) * Matrix.CreateTranslation(RightRect.X * 2 + fw, 0)))
                        g.DrawImage(sonic, RightRect);
            }
        }

        public Rect LeftRect, RightRect, BallRect;

        protected override void OnPointerPressed(PointerPressedEventArgs e)
        {
            var at = e.GetPosition(this);
            if (LeftRect.Contains(at)) w.jumpAt["left"] = w.T;
            else if (RightRect.Contains(at)) w.jumpAt["right"] = w.T;
            else if (BallRect.Contains(at)) w.NextTip();
        }

        protected override void OnPointerMoved(PointerEventArgs e)
        {
            var at = e.GetPosition(this);
            Cursor = LeftRect.Contains(at) || RightRect.Contains(at) || BallRect.Contains(at) ? BallHand : BallCursor;
        }
    }

    // A four-point sparkle, as the Smash Ball's trail.
    static void Sparkle(DrawingContext g, double cx, double cy, double r, Color color)
    {
        double n = r * 0.3;
        var star = new StreamGeometry();
        using (var c = star.Open())
        {
            c.BeginFigure(new Point(cx, cy - r), true);
            foreach (var p in new[] { new Point(cx + n, cy - n), new Point(cx + r, cy), new Point(cx + n, cy + n), new Point(cx, cy + r),
                                      new Point(cx - n, cy + n), new Point(cx - r, cy), new Point(cx - n, cy - n) })
                c.LineTo(p);
            c.EndFigure(true);
        }
        g.DrawGeometry(new SolidColorBrush(color), null, star);
    }

    // ----- Art (tools/make_launcher_art.py, from the game's files), clicks and the pointer -----
    static readonly Dictionary<string, Bitmap?> art = new();

    public static Bitmap? Art(string name)
    {
        if (art.TryGetValue(name, out var cached)) return cached;
        using var stream = typeof(MainWindow).Assembly.GetManifestResourceStream("art." + name + ".png");
        return art[name] = stream == null ? null : new Bitmap(stream);
    }

    readonly Dictionary<string, double> jumpAt = new();

    // How far into a click-triggered move (0..1), or -1 when none is playing.
    double Hop(string name, double seconds) => jumpAt.TryGetValue(name, out var at) && T - at < seconds ? (T - at) / seconds : -1;

    // The pointer: the Smash Ball (a little smaller with a sparkle over anything clickable).
    public static Cursor BallCursor = new(StandardCursorType.Arrow), BallHand = new(StandardCursorType.Hand);

    static Cursor MakeBallCursor(bool clickable)
    {
        if (Art("smash_ball") is not { } ball) return new Cursor(clickable ? StandardCursorType.Hand : StandardCursorType.Arrow);
        var bitmap = new RenderTargetBitmap(new PixelSize(32, 32));
        using (var g = bitmap.CreateDrawingContext())
        {
            double size = clickable ? 24 : 28;
            g.DrawImage(ball, new Rect(0, 0, size, size));
            if (clickable) Sparkle(g, 25.6, 25.6, 6.4, Gold);
        }
        return new Cursor(bitmap, new PixelPoint(4, 4));  // the ball's upper-left edge points
    }

    // A fighter stands under the menu buttons when there is room; a click spins them round and the next one turns in.
    static readonly string[] NavFighters = { "kirby", "pikachu", "fox", "samus", "pit", "metaknight", "ike", "snake" };
    int navFighter;
    bool navTurned;

    sealed class NavFighterView : Control
    {
        readonly MainWindow w;
        Rect fighterRect;
        public NavFighterView(MainWindow window) { w = window; }

        public override void Render(DrawingContext g)
        {
            double t = w.T;
            double top = w.navButtons.Bounds.Bottom + 10, bottom = Bounds.Height - 10;
            double room = bottom - top - 12;
            if (room < 110) { fighterRect = default; return; }
            double h = Math.Min(200, room), wd = h * 4 / 5;
            double spin = w.Hop("nav", 0.9);
            if (spin >= 0.5 && !w.navTurned) { w.navFighter = (w.navFighter + 1) % NavFighters.Length; w.navTurned = true; }
            if (spin < 0) w.navTurned = false;
            // The spin squeezes them to a sliver (a turn), the next fighter coming round from the halfway point.
            double turn = spin < 0 ? 1 : Math.Cos(spin * Math.PI * 2);
            double sw = Math.Max(1, wd * Math.Abs(turn));
            double y = bottom - h - 12 - (Math.Sin(t * 1.6) + 1) * 5;
            double cx = (Bounds.Width + 16) / 2;
            fighterRect = new Rect(cx - wd / 2, y, wd, h);
            double shadowW = wd * 2 / 3 - (Math.Sin(t * 1.6) + 1) * 8;
            g.DrawEllipse(new SolidColorBrush(Color.FromArgb(70, 0, 0, 0)), null, new Rect(cx - shadowW / 2, bottom - 10, shadowW, 12));
            if (Art("fighter_" + NavFighters[w.navFighter]) is { } image)
            {
                var place = new Rect(cx - sw / 2, y, sw, h);
                if (turn < 0)
                    using (g.PushTransform(Matrix.CreateScale(-1, 1) * Matrix.CreateTranslation(place.X * 2 + sw, 0)))
                        g.DrawImage(image, place);
                else g.DrawImage(image, place);
            }
        }

        // Only the fighter takes clicks; the rest of the column passes them on.
        protected override void OnPointerPressed(PointerPressedEventArgs e)
        {
            if (fighterRect.Contains(e.GetPosition(this)) && w.Hop("nav", 0.9) < 0) w.jumpAt["nav"] = w.T;
        }

        protected override void OnPointerMoved(PointerEventArgs e) => Cursor = fighterRect.Contains(e.GetPosition(this)) ? BallHand : BallCursor;
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
    TextBlock? tipLabel;
    int tipIndex;

    void NextTip()
    {
        tipIndex = (tipIndex + 1) % Tips.Length;
        if (tipLabel != null) tipLabel.Text = "Tip: " + Tips[tipIndex];
    }

    // A card holding one page's controls, top to bottom; it scrolls when they are taller than the window.
    // `art`: the heading drawn in the game's title font (text_head_<art>.png).
    static (Control card, StackPanel body) NewCard(string heading, string art)
    {
        var body = new StackPanel();
        Control title = Art("text_head_" + art) is { } image
            ? new Image { Source = image, Width = image.PixelSize.Width / 2.0, Height = image.PixelSize.Height / 2.0, HorizontalAlignment = HorizontalAlignment.Left }
            : new TextBlock { Text = heading, FontSize = Heading, FontWeight = FontWeight.Bold };
        title.Margin = new Thickness(0, 0, 0, 8);
        body.Children.Add(title);
        var card = new Border
        {
            Background = new SolidColorBrush(Card), BorderBrush = EdgeBrush, BorderThickness = new Thickness(3), CornerRadius = new CornerRadius(14),
            Margin = new Thickness(2, 2, 8, 8),
            Child = new ScrollViewer { Content = body, Padding = new Thickness(22, 16, 22, 16) },
        };
        return (card, body);
    }

    static TextBlock Note(string text, double width = 700) => new()
    {
        Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = width, HorizontalAlignment = HorizontalAlignment.Left, Margin = new Thickness(0, 6, 0, 6),
        Foreground = new SolidColorBrush(Dim),
    };

    static TextBlock Subheading(string text) => new() { Text = text, FontSize = 16, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 10, 0, 2) };

    static PaperButton Btn(string text, Action? onClick, Color? back = null)
    {
        var b = new PaperButton(text, back ?? Orange, Body);
        if (onClick != null) b.Click += onClick;
        return b;
    }

    static StackPanel Row(params Control[] children)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal };
        foreach (var c in children) row.Children.Add(c);
        return row;
    }

    // ----- Play -----
    Control BuildPlayPage()
    {
        var (card, body) = NewCard("Ready to brawl?", "play");
        playButton = new PaperButton("PLAY", Red, PlaySize) { TextBrush = Brushes.White, Padding = new Thickness(60, 8, 60, 8), Margin = new Thickness(0, 6, 10, 6) };
        playButton.Click += () => Launch();
        playButton.ShowImage(Art("text_play"));
        playButton.Padding = new Thickness(60, 6, 60, 6);
        body.Children.Add(playButton);
        playStatus = Note("");
        playStatus.FontWeight = FontWeight.Bold;
        playStatus.Foreground = SnowBrush;
        body.Children.Add(playStatus);
        AddDiscSetup(body);
        body.Children.Add(gameNote = Note("The game starts borderless at 1080p in its own 16:9 mode. " +
                               "In the game, F10 opens the settings menu (resolution, aspect, controllers, audio, online play) and F11 switches between fullscreen and a window."));
        // Tips: click the bubble (or the Smash Ball in the sky) for another one.
        tipLabel = new TextBlock { TextWrapping = TextWrapping.Wrap, MaxWidth = 600, Foreground = SnowBrush };
        var bubble = new Border
        {
            Child = tipLabel, Background = new SolidColorBrush(Night), BorderBrush = EdgeBrush, BorderThickness = new Thickness(1),
            Padding = new Thickness(10, 8, 10, 8), Margin = new Thickness(0, 4, 0, 0), Cursor = BallHand, VerticalAlignment = VerticalAlignment.Center,
        };
        bubble.PointerPressed += (_, _) => NextTip();
        var tipRow = Row(new Image { Source = Art("smash_ball"), Width = 44, Height = 44, Margin = new Thickness(0, 0, 8, 0) }, bubble);
        tipRow.Margin = new Thickness(0, 10, 0, 6);
        tipIndex = Random.Shared.Next(Tips.Length) - 1;
        NextTip();
        body.Children.Add(tipRow);
        body.Children.Add(Row(
            Btn("Open game folder", () => Open(play), Card),
            Btn("Open saves folder", () => Open(Directory.CreateDirectory(SaveDataDir()).FullName), Card),
            Btn("Import Wii / Dolphin save...", ImportSave, Card)));
        return card;
    }

    static void Open(string folder)
    {
        try { Process.Start(new ProcessStartInfo("xdg-open", $"\"{folder}\"") { UseShellExecute = false }); } catch (Exception) { }
    }

    // A Dolphin save folder, or a data.bin a homebrew save manager exported from a Wii (Brawl's save is no-copy).
    async void ImportSave()
    {
        const string saveDir = "Wii/title/00010000/52534245/data";
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var dolphin = new[] { ".local/share/dolphin-emu", ".var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu", ".dolphin-emu" }
            .Select(d => Path.Combine(home, d, saveDir)).FirstOrDefault(Directory.Exists);
        var file = await PickFile("Choose a Super Smash Bros. Brawl save: autosv0.bin in a Dolphin save folder, or a data.bin",
            "Dolphin or Wii save (autosv0.bin, data.bin)", new[] { "autosv0.bin", "data.bin" }, dolphin);
        if (file == null) return;
        var dataDir = SaveDataDir();
        if (Directory.Exists(dataDir) && Directory.EnumerateFiles(dataDir).Any() &&
            !await Message("Import save", "This replaces your current save files. They are kept as a backup next to them (data.backup-<date>).\n\nImport the save?", cancel: true)) return;
        try
        {
            var files = WiiSave.Import(file, dataDir);
            await Message("Import save", "Imported " + string.Join(", ", files) + ".");
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException or System.Security.Cryptography.CryptographicException)
        {
            await Message("Import save", "The save could not be imported:\n" + e.Message);
        }
    }

    // The desktop's file picker (XDG portal or GTK); a typed path where neither exists.
    async Task<string?> PickFile(string title, string typeName, string[] patterns, string? startDir)
    {
        try
        {
            if (StorageProvider.CanOpen)
            {
                var options = new FilePickerOpenOptions
                {
                    Title = title, AllowMultiple = false,
                    FileTypeFilter = new[] { new FilePickerFileType(typeName) { Patterns = patterns }, FilePickerFileTypes.All },
                };
                if (startDir != null) options.SuggestedStartLocation = await StorageProvider.TryGetFolderFromPathAsync(startDir);
                var files = await StorageProvider.OpenFilePickerAsync(options);
                return files.Count > 0 ? files[0].TryGetLocalPath() : null;
            }
        }
        catch (Exception) { }  // no portal or GTK after all: ask for the path instead
        var path = await AskPath(title);
        return string.IsNullOrWhiteSpace(path) ? null : path.Trim().Trim('"', '\'');
    }

    // The desktop's folder picker; a typed path where there is none.
    async Task<string?> PickFolder(string title)
    {
        try
        {
            if (StorageProvider.CanPickFolder)
            {
                var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = title, AllowMultiple = false });
                return folders.Count > 0 ? folders[0].TryGetLocalPath() : null;
            }
        }
        catch (Exception) { }
        var path = await AskPath(title);
        return string.IsNullOrWhiteSpace(path) ? null : path.Trim().Trim('"', '\'');
    }

    async Task<string?> AskPath(string title)
    {
        var box = new TextBox { Width = 560, Watermark = "/home/you/Downloads/Super Smash Bros. Brawl.iso" };
        var dialog = Dialog(title, Note(title + "\n\nType or paste the file's full path:", 560), box, out var ok, out var cancel);
        ok.Click += () => dialog.Close(box.Text);
        cancel.Click += () => dialog.Close(null);
        return await dialog.ShowDialog<string?>(this);
    }

    // The Windows launcher's MessageBox: OK, or OK and Cancel.
    async Task<bool> Message(string title, string text, bool cancel = false)
    {
        var dialog = Dialog(title, Note(text, 520), null, out var ok, out var no);
        no.IsVisible = cancel;
        ok.Click += () => dialog.Close(true);
        no.Click += () => dialog.Close(false);
        return await dialog.ShowDialog<bool>(this);
    }

    static Window Dialog(string title, Control text, Control? extra, out PaperButton ok, out PaperButton cancel)
    {
        var body = new StackPanel { Margin = new Thickness(22, 16, 22, 16) };
        body.Children.Add(text);
        if (extra != null) body.Children.Add(extra);
        ok = Btn("OK", null);
        cancel = Btn("Cancel", null, Card);
        var buttons = Row(ok, cancel);
        buttons.HorizontalAlignment = HorizontalAlignment.Right;
        body.Children.Add(buttons);
        return new Window
        {
            Title = title, Content = body, SizeToContent = SizeToContent.WidthAndHeight, CanResize = false, ShowInTaskbar = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner, Background = new SolidColorBrush(Night), Foreground = SnowBrush,
            FontFamily = Font, FontSize = Body,
        };
    }

    // `online`: what the game's Online menu does at start (BRAWL_ONLINE: "host:<n>", "host-public:<n>", "browse",
    // "join:<code>", "menu"), or null for a normal start.
    async void Launch(string? online = null)
    {
        var (ok, text, _) = DiscStatus();
        if (!ok)
        {
            await Message("Brawl Recompiled", text + "\n\nChoose your disc image with the button under PLAY first.");
            ShowPage(pages[0].button);
            return;
        }
        // A disc put in Disc/DATA by hand (a Dolphin-extracted folder) has no [paths] dvd_root yet, and the game has no
        // folder picker on Linux to ask for it: point the game at the folder the launcher found.
        if (config.Get("paths", "dvd_root") == null) { config.SetString("paths", "dvd_root", "../../Disc/DATA"); config.Save(); }
        if (!File.Exists(gameExe))
        {
            await Message("Brawl Recompiled", "The game files (the engine folder) are missing. Unpack the whole download again, keeping the engine folder next to this launcher.");
            return;
        }
        Gamepads.Release();
        var start = new ProcessStartInfo(gameExe) { WorkingDirectory = play, UseShellExecute = false };
        // The game's few bundled libraries (libpng) sit beside it.
        start.Environment["LD_LIBRARY_PATH"] = play + (Environment.GetEnvironmentVariable("LD_LIBRARY_PATH") is { Length: > 0 } more ? ":" + more : "");
        if (online != null) start.Environment["BRAWL_ONLINE"] = online;
        Process? game = null;
        try { game = Process.Start(start); } catch (Exception e) { await Message("Brawl Recompiled", "The game could not start: " + e.Message); }
        if (game != null)
        {
            game.EnableRaisingEvents = true;
            game.Exited += (_, _) => Dispatcher.UIThread.Post(Gamepads.Resume);
            WindowState = WindowState.Minimized;
        }
        else Gamepads.Resume();
    }

    // ----- Online -----
    Control BuildOnlinePage()
    {
        var (card, body) = NewCard("Online", "online");
        body.Children.Add(Note("Play Brawl Recompiled with friends over the internet, or on your own network. Host a room and share its code, " +
                               "find a room, or join a friend's code. In the game, F9 opens the same menu at any time.\n" +
                               "Everyone needs the same game: vanilla, or the same mod (Mods page). Each player uses the controller in their own port 1."));
        TextBox Field(string section, string key, string placeholder, double width)
        {
            var box = new TextBox { Width = width, Watermark = placeholder, Text = config.Get(section, key) ?? "", Margin = new Thickness(3, 4, 3, 4) };
            box.LostFocus += (_, _) =>
            {
                var text = (box.Text ?? "").Trim();
                if (text.Length == 0) config.Remove(section, key); else config.SetString(section, key, text);
                config.Save();
            };
            return box;
        }
        StackPanel Labeled(string label, Control control) =>
            Row(new TextBlock { Text = label, Width = 200, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, control);

        body.Children.Add(Labeled("Your name", Field("online", "name", Environment.MachineName, 260)));

        body.Children.Add(Subheading("Host a room"));
        var players = new ComboBox { Width = 140, ItemsSource = new[] { "2 players", "3 players", "4 players" }, SelectedIndex = 0, Margin = new Thickness(3, 4, 3, 4) };
        body.Children.Add(Labeled("Players", players));
        var listed = new CheckBox { Content = "List it on the online server, so anyone can find it (friends can always join with its code)", IsChecked = true, Margin = new Thickness(0, 4, 0, 4) };
        body.Children.Add(listed);
        body.Children.Add(Row(Btn("HOST A ROOM", () => Launch((listed.IsChecked == true ? "host-public:" : "host:") + (players.SelectedIndex + 2)))));

        body.Children.Add(Subheading("Join a room"));
        var code = new TextBox { Width = 170, Watermark = "Room code", Margin = new Thickness(14, 6, 3, 6), VerticalAlignment = VerticalAlignment.Center };
        body.Children.Add(Row(Btn("FIND ROOMS", () => Launch("browse")), code,
            Btn("JOIN", () => { var c = (code.Text ?? "").Trim(); if (c.Length > 0) Launch("join:" + c.ToUpperInvariant()); }, Card)));
        body.Children.Add(Note("A room code is 5 letters and digits, shown in the host's Online menu. An IP address or Tailscale name works too."));

        body.Children.Add(Subheading("Server"));
        body.Children.Add(Labeled("Online server", Field("online", "server", "the built-in server", 320)));
        body.Children.Add(Note("Leave this empty to use the built-in server. Running your own? Put its address here: an https:// address for a " +
                               "matchmaking Worker (server/worker), or host:port for a relay (server/brawl_online_server.py). Rooms on your network and over Tailscale always work."));
        return card;
    }

    // ----- Display -----
    Control BuildDisplayPage()
    {
        var (card, body) = NewCard("Display", "display");
        body.Children.Add(Choice("Display mode", "display_mode", new[] { ("Borderless fullscreen", "\"borderless\""), ("Windowed", "\"windowed\""), ("Exclusive fullscreen", "\"exclusive\"") }));
        body.Children.Add(Choice("Resolution", "resolution_multiplier", new[] { ("1080p", "2.25"), ("4K (2160p, needs a graphics card)", "4.5"), ("Match the screen", "0"), ("1440p", "3"), ("720p", "1.5"), ("480p (original)", "1") }));
        // The game's own 16:9 mode (its widescreen switch) or 4:3; "fill" stretches the 16:9 camera to wider screens.
        body.Children.Add(Choice("Aspect ratio", "aspect_mode", new[] { ("16:9 (the game's own widescreen)", "1"), ("Fill the screen (ultrawide)", "2"), ("Original 4:3", "0") },
            also: value => config.Set("video", "widescreen", value == "0" ? "false" : "true")));
        body.Children.Add(Choice("Frame interpolation", "frame_interpolation_fps", new[] { ("120 FPS (on a 120 Hz or faster screen)", "120"), ("Off (60 FPS)", "0"), ("180 FPS (on a 180 Hz or faster screen)", "180"), ("240 FPS (on a 240 Hz or faster screen)", "240") }));
        body.Children.Add(Choice("Vsync", "vsync", new[] { ("On (no screen tearing)", "true"), ("Off", "false") }));
        body.Children.Add(Choice("FPS counter", "show_fps", new[] { ("Hidden", "false"), ("Shown", "true") }));
        body.Children.Add(Note("Changes are saved at once and used the next time the game starts. The same settings are in the game's F10 menu. " +
                               "Frame interpolation inserts frames between the game's 60 fps (it needs a display faster than 60 Hz); turn it off if you see artifacts."));
        return card;
    }

    Control Choice(string label, string key, (string label, string value)[] items, string section = "video", Action<string>? also = null)
    {
        var combo = new ComboBox { Width = 300, ItemsSource = items.Select(i => i.label).ToList(), Margin = new Thickness(3, 4, 3, 4) };
        string current = config.Get(section, key) ?? "";
        int index = Array.FindIndex(items, i => i.value.Trim('"') == current || (double.TryParse(i.value, CultureInfo.InvariantCulture, out var a) && double.TryParse(current, CultureInfo.InvariantCulture, out var b) && a == b));
        combo.SelectedIndex = index < 0 ? 0 : index;
        combo.SelectionChanged += (_, _) =>
        {
            if (combo.SelectedIndex < 0) return;
            var value = items[combo.SelectedIndex].value;
            config.Set(section, key, value);
            also?.Invoke(value);
            config.Save();
        };
        var row = Row(new TextBlock { Text = label, Width = 200, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, combo);
        row.Margin = new Thickness(0, 4, 0, 4);
        return row;
    }

    // ----- Controls -----
    // Every gamepad is a GameCube controller on its port (runtime hle/input/pad.cpp), laid out automatically for its
    // type. The Gamepad view changes one port's buttons through the game's [controller] expr_<port>_<control>
    // expressions (runtime input_bindings.cpp, Dolphin's input names). The Keyboard view maps keys to player 1's controller.
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

    // A 4-column table: label, button, label, button; six controls per column pair.
    static Grid ControlGrid() => new()
    {
        ColumnDefinitions = new ColumnDefinitions("Auto,Auto,Auto,Auto"), RowDefinitions = new RowDefinitions("Auto,Auto,Auto,Auto,Auto,Auto"),
        Margin = new Thickness(0, 4, 0, 4), HorizontalAlignment = HorizontalAlignment.Left,
    };

    static void Place(Grid grid, Control control, int column, int row)
    {
        Grid.SetColumn(control, column);
        Grid.SetRow(control, row);
        grid.Children.Add(control);
    }

    Control BuildControlsPage()
    {
        var (card, body) = NewCard("Controls", "controls");
        var inputCombo = new ComboBox { Width = 460, Margin = new Thickness(3, 4, 3, 4) };
        var inputRow = Row(new TextBlock { Text = "Input", Width = 110, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, inputCombo);
        inputRow.Margin = new Thickness(0, 0, 0, 6);
        body.Children.Add(inputRow);
        var gamepad = new StackPanel();
        var keyboard = new StackPanel();
        body.Children.Add(gamepad);
        body.Children.Add(keyboard);

        // Gamepad: one button per GameCube control; click it, then press the controller input to bind (Escape or a click cancels).
        int style = 0, port = 1;
        uint padId = 0;
        gamepad.Children.Add(Note("This controller is a GameCube controller on its port and is set up automatically: the face buttons by position, " +
                                  "the bumpers L and R, the right trigger Z, the left stick to move and the right stick as the C-stick. " +
                                  "To give a control another button as well, click it and press that button on your controller (Escape cancels); " +
                                  "the automatic button keeps working too."));
        var padTable = ControlGrid();
        gamepad.Children.Add(padTable);
        PaperButton? capturingPad = null;
        var heldAtStart = new HashSet<string>();
        var padTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
        string ExprKey(string control) => $"expr_{port}_{control}";
        void FillPadTable()
        {
            padTable.Children.Clear();
            for (int i = 0; i < GcControls.Length; i++)
            {
                var (key, label) = GcControls[i];
                Place(padTable, new TextBlock { Text = label, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 12, 12, 0) }, (i / 6) * 2, i % 6);
                string? custom = config.Get("controller", ExprKey(key));
                var b = Btn(custom == null ? "Automatic" : PadLabel(style, custom), null, custom == null ? Card : Hover);
                b.Width = 240;
                b.Height = 40;
                b.Margin = new Thickness(0, 4, 18, 4);
                b.Tag = key;
                b.Click += () =>
                {
                    if (capturingPad != null) { EndPadCapture(null); return; }
                    capturingPad = b;
                    b.Text = "Press a button...";
                    b.Back = Orange;
                    b.TextBrush = new SolidColorBrush(Night);
                    heldAtStart = Gamepads.Active(padId);
                    padTimer.Start();
                };
                Place(padTable, b, (i / 6) * 2 + 1, i % 6);
            }
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
        gamepad.Children.Add(Row(Btn("Reset to automatic", () =>
        {
            EndPadCapture(null);
            foreach (var (key, _) in GcControls) config.Remove("controller", ExprKey(key));
            config.Save();
            FillPadTable();
        }, Card)));

        // Keyboard: player 1's GameCube controller (Config.toml [gc_keyboard], applied by the runtime's pad start).
        var useKeyboard = new CheckBox
        {
            Content = "Use the keyboard and mouse as player 1's controller", FontWeight = FontWeight.Bold, Margin = new Thickness(0, 4, 0, 4),
            IsChecked = config.Get("gc_keyboard", "enabled") == "true",
        };
        keyboard.Children.Add(useKeyboard);
        keyboard.Children.Add(Note("Click a control, then press the key for it, or click the control again with the mouse button you want " +
                                   "(Escape cancels, Delete clears). It replaces the gamepad on port 1; the game's F10 menu has the same settings."));
        var keyTable = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("Auto,Auto,Auto,Auto"), RowDefinitions = new RowDefinitions(string.Join(",", Enumerable.Repeat("Auto", 10))),
            Margin = new Thickness(0, 4, 0, 4), HorizontalAlignment = HorizontalAlignment.Left,
        };
        var keyButtons = new List<PaperButton>();
        for (int i = 0; i < KeyboardControls.Length; i++)
        {
            var (key, label, defaults) = KeyboardControls[i];
            Place(keyTable, new TextBlock { Text = label, FontWeight = FontWeight.Bold, Margin = new Thickness(0, 12, 12, 0) }, (i / 10) * 2, i % 10);
            var b = Btn(config.Get("gc_keyboard", key) is { } bound ? KeyText(bound) : defaults, null, Card);
            b.Width = 200;
            b.Height = 38;
            b.Margin = new Thickness(0, 3, 24, 3);
            b.Tag = key;
            b.Click += () => { if (capturingKey == null) BeginKeyCapture(b); };
            // A mouse button pressed on the control being set binds that mouse button (the click that started it does not).
            b.AddHandler(PointerPressedEvent, (_, e) =>
            {
                if (capturingKey != b || keyCaptureStarted.ElapsedMilliseconds < 300) return;
                var props = e.GetCurrentPoint(b).Properties;
                EndKeyCapture(props.IsLeftButtonPressed ? "Mouse Left" : props.IsRightButtonPressed ? "Mouse Right" : props.IsMiddleButtonPressed ? "Mouse Middle" : null);
                e.Handled = true;
            }, RoutingStrategies.Tunnel);
            keyButtons.Add(b);
            Place(keyTable, b, (i / 10) * 2 + 1, i % 10);
        }
        keyboard.Children.Add(keyTable);
        useKeyboard.IsCheckedChanged += (_, _) =>
        {
            bool on = useKeyboard.IsChecked == true;
            config.Set("gc_keyboard", "enabled", on ? "true" : "false");
            // The whole layout goes to Config.toml once the keyboard is turned on, so the game applies exactly what is shown.
            if (on) foreach (var (key, _, defaults) in KeyboardControls) if (config.Get("gc_keyboard", key) == null) config.SetString("gc_keyboard", key, defaults);
            config.Save();
        };
        keyboard.Children.Add(Row(Btn("Reset to the WASD + mouse layout", () =>
        {
            foreach (var (key, _, defaults) in KeyboardControls) config.SetString("gc_keyboard", key, defaults);
            config.Save();
            foreach (var b in keyButtons) b.Text = KeyboardControls.First(c => c.key == (string)b.Tag!).defaults;
        }, Card)));

        // The input list: connected pads first (in port order), then the keyboard.
        var shown = new List<(uint id, string name, int style)>();
        var names = new List<string>();
        bool filling = false;
        void ShowSelected()
        {
            int i = inputCombo.SelectedIndex;
            bool isPad = i >= 0 && i < shown.Count;
            gamepad.IsVisible = isPad;
            keyboard.IsVisible = !isPad;
            if (isPad) { EndPadCapture(null); style = shown[i].style; padId = shown[i].id; port = i + 1; FillPadTable(); }
        }
        void RefreshInputs()
        {
            var pads = Gamepads.Connected();
            if (pads.Count == 0 && screenshotDir != null) pads.Add((0, "Test controller (screenshot mode)", 0));
            if (pads.SequenceEqual(shown) && names.Count > 0) return;
            uint? selectedId = inputCombo.SelectedIndex >= 0 && inputCombo.SelectedIndex < shown.Count ? shown[inputCombo.SelectedIndex].id : null;
            bool keyboardSelected = names.Count > 0 && inputCombo.SelectedIndex == shown.Count;
            shown.Clear();
            shown.AddRange(pads);
            names.Clear();
            for (int i = 0; i < pads.Count; i++) names.Add($"Port {i + 1}: {pads[i].name}  ({new[] { "Xbox", "PlayStation", "Nintendo" }[pads[i].style]} layout)");
            names.Add(pads.Count == 0 ? "Keyboard  (no controller connected - plug one in and it appears here)" : "Keyboard");
            int keep = selectedId is uint id ? shown.FindIndex(p => p.id == id) : -1;
            filling = true;
            inputCombo.ItemsSource = names.ToList();
            inputCombo.SelectedIndex = keyboardSelected ? shown.Count : keep >= 0 ? keep : 0;
            filling = false;
            ShowSelected();
        }
        inputCombo.SelectionChanged += (_, _) => { if (!filling) ShowSelected(); };
        RefreshInputs();
        var poll = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        poll.Tick += (_, _) => { if (card.IsVisible) RefreshInputs(); };
        poll.Start();
        return card;
    }

    // The keyboard's GameCube controls (runtime pad.cpp ApplyKeyboardConfig reads the same keys), with the in-game
    // menu's WASD + mouse preset as defaults. Same table as the Windows launcher.
    static readonly (string key, string label, string defaults)[] KeyboardControls =
    {
        ("a", "A", "Mouse Left"), ("b", "B", "Space"), ("x", "X", "E"), ("y", "Y", "Q"), ("z", "Z", "Mouse Middle"),
        ("l", "L (shield)", "Left Shift"), ("r", "R (shield)", "Mouse Right"), ("start", "Start", "Return"),
        ("dpad_up", "D-pad up", "Up"), ("dpad_down", "D-pad down", "Down"), ("dpad_left", "D-pad left", "Left"), ("dpad_right", "D-pad right", "Right"),
        ("stick_up", "Stick up", "W"), ("stick_down", "Stick down", "S"), ("stick_left", "Stick left", "A"), ("stick_right", "Stick right", "D"),
        ("cstick_up", "C-stick up", "I"), ("cstick_down", "C-stick down", "K"), ("cstick_left", "C-stick left", "J"), ("cstick_right", "C-stick right", "L"),
    };

    PaperButton? capturingKey;
    string? captureText;
    readonly Stopwatch keyCaptureStarted = new();

    static string KeyText(string key) => key.Length == 0 ? "None" : key;

    void BeginKeyCapture(PaperButton button)
    {
        capturingKey = button;
        captureText = button.Text;
        keyCaptureStarted.Restart();
        button.Text = "Press a key...";
        button.Back = Orange;
        button.TextBrush = new SolidColorBrush(Night);
    }

    // name: the binding (an SDL key name or "Mouse ..."), "" to clear it, null to cancel.
    void EndKeyCapture(string? name)
    {
        if (capturingKey == null) return;
        if (name != null)
        {
            config.SetString("gc_keyboard", (string)capturingKey.Tag!, name);
            config.Save();
            captureText = KeyText(name);
        }
        capturingKey.Text = captureText!;
        capturingKey.Back = Card;
        capturingKey.TextBrush = SnowBrush;
        capturingKey = null;
    }

    void OnKey(object? sender, KeyEventArgs e)
    {
        if (capturingKey != null)
        {
            e.Handled = true;
            if (e.Key == Key.Escape) EndKeyCapture(null);
            else if (e.Key == Key.Delete) EndKeyCapture("");
            else if (SdlName(e.Key) is { } name) EndKeyCapture(name);
            return;  // a key SDL has no name for here: keep waiting
        }
        if (e.Key == Key.Escape && cancelPadCapture?.Invoke() == true) e.Handled = true;
    }

    // SDL scancode names, as the runtime reads them (SDL_GetScancodeFromName).
    static string? SdlName(Key k) => k switch
    {
        >= Key.A and <= Key.Z => k.ToString(),
        >= Key.D0 and <= Key.D9 => ((char)('0' + (k - Key.D0))).ToString(),
        >= Key.NumPad0 and <= Key.NumPad9 => "Keypad " + (k - Key.NumPad0),
        >= Key.F1 and <= Key.F9 => k.ToString(),
        Key.Up => "Up", Key.Down => "Down", Key.Left => "Left", Key.Right => "Right",
        Key.Space => "Space", Key.Return => "Return", Key.Tab => "Tab", Key.Back => "Backspace", Key.Insert => "Insert",
        Key.Home => "Home", Key.End => "End", Key.PageUp => "PageUp", Key.PageDown => "PageDown", Key.CapsLock => "CapsLock",
        Key.LeftShift => "Left Shift", Key.RightShift => "Right Shift", Key.LeftCtrl => "Left Ctrl", Key.RightCtrl => "Right Ctrl",
        Key.LeftAlt => "Left Alt", Key.RightAlt => "Right Alt",
        Key.OemMinus => "-", Key.OemPlus => "=", Key.OemOpenBrackets => "[", Key.OemCloseBrackets => "]", Key.OemComma => ",",
        Key.OemPeriod => ".", Key.OemQuestion => "/", Key.OemSemicolon => ";", Key.OemQuotes => "'", Key.OemTilde => "`",
        _ => null,
    };

    // ----- Game disc (part of the Play page) -----
    // One button sets the disc up (or changes it later); extraction progress shows under it, and the dump
    // instructions only while no disc is set up.
    void AddDiscSetup(StackPanel body)
    {
        var progress = new ProgressBar { Width = 640, Height = 22, Margin = new Thickness(0, 6, 0, 2), IsVisible = false, HorizontalAlignment = HorizontalAlignment.Left };
        var progressText = Note("");
        progressText.IsVisible = false;
        void Say(string text) { progressText.Text = text; progressText.IsVisible = text.Length > 0; }  // no empty gap
        var choose = chooseDisc = Btn("Choose disc image (.iso, .ciso, .wbfs)...", null);
        choose.HorizontalAlignment = HorizontalAlignment.Left;
        choose.Click += async () =>
        {
            var file = await PickFile("Choose your Super Smash Bros. Brawl disc image", "Wii disc images (*.iso, *.ciso, *.wbfs)",
                new[] { "*.iso", "*.ciso", "*.wbfs", "*.ISO", "*.CISO", "*.WBFS" }, null);
            if (file == null) return;
            WiiDisc disc;
            try { disc = WiiDisc.Open(file); }
            catch (Exception ex) { Say("Could not read that image: " + ex.Message); return; }
            using (disc)
            {
                if (disc.Problem() is { Length: > 0 } problem) { Say(problem); return; }
                choose.IsEnabled = false;
                progress.IsVisible = true;
                var staging = discDir + ".new";
                try
                {
                    if (Directory.Exists(staging)) Directory.Delete(staging, true);
                    var report = new Progress<(int done, int total, string name)>(p =>
                    {
                        progress.Maximum = p.total;
                        progress.Value = Math.Min(p.done, p.total);
                        Say($"Extracting {p.done}/{p.total}  {p.name}");
                    });
                    await Task.Run(() => disc.Extract(staging, (d, t, n) => ((IProgress<(int, int, string)>)report).Report((d, t, n))));
                    if (disc.Holes > 0) throw new InvalidDataException("the image is incomplete (blocks missing); dump the disc again.");
                    if (Directory.Exists(discDir)) Directory.Delete(discDir, true);
                    Directory.Move(staging, discDir);
                    config.SetString("paths", "dvd_root", "../../Disc/DATA");  // relative to engine/UserData, so the folder can move
                    config.Save();
                    Say("Done. The disc is extracted and ready.");
                }
                catch (Exception ex)
                {
                    Say("Extraction failed: " + ex.Message);
                }
                finally
                {
                    choose.IsEnabled = true;
                    progress.IsVisible = false;
                    RefreshStatus();
                }
            }
        };
        body.Children.Add(choose);
        body.Children.Add(progress);
        body.Children.Add(progressText);
        body.Children.Add(dumpHelp = Note(
            "This port runs your own copy of Super Smash Bros. Brawl (USA, RSBE01). It needs a disc image of the disc you own, " +
            "which the launcher decrypts and extracts once into its Disc folder (about 7 GB; the disc is dual-layer, so give it a few minutes).\n\n" +
            "Making a legal image of your disc (Wii or Wii U consoles can read the disc; PC drives can't):\n" +
            "1. On a Wii with the Homebrew Channel, run CleanRip (or USB Loader GX's install option).\n" +
            "2. Insert Super Smash Bros. Brawl and dump it to an SD card or USB drive. CleanRip writes a .iso " +
            "(it may be split into .part files: join them with  cat game.part* > game.iso ); USB Loader GX writes a .wbfs.\n" +
            "3. Copy the file to this PC and choose it with the button above. .iso, .ciso and .wbfs all work.\n" +
            "The USA release, revision 1, works (the European and Japanese discs do not). " +
            "A Dolphin-extracted folder also works: put it in Disc/DATA next to the launcher."));
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
        playStatus.Foreground = new SolidColorBrush(ok ? Snow : Orange);
        chooseDisc.Text = ok ? "Change disc..." : "Choose disc image (.iso, .ciso, .wbfs)...";
        chooseDisc.Back = ok ? Card : Orange;
        chooseDisc.TextBrush = ok ? SnowBrush : new SolidColorBrush(Night);
        dumpHelp.IsVisible = !ok;
        gameNote.IsVisible = ok;
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
        var (card, body) = NewCard("Mods", "mods");
        body.Children.Add(Subheading("SD card mods (REX, Project+Ex, Project+, Brawl Minus, Legacy XP)"));
        body.Children.Add(Note("The SD card folder is the game's SD card, the way a Wii sees one. Add a mod with the buttons below (choose its Wii / console download: the .zip, or the folder you extracted it to), or copy a build's card folder from that download " +
                               "download into it: rex_ for REX (the biggest roster), P+Ex, Project+, Minusery (Brawl Minus) or LegacyXP. Its Gecko codes " +
                               "(RSBE01.GCT and gc.txt) load with it, and the game's own SD features (snapshots, replays, custom stages) use the card too. " +
                               "The card image is rebuilt when the folder changes; a big build takes a few minutes the first time."));
        sdStatus = Note("");
        sdStatus.Foreground = SnowBrush;
        body.Children.Add(sdStatus);
        body.Children.Add(Row(Btn("Open SD card folder", () => Open(Directory.CreateDirectory(sdDir).FullName))));
        // Which build's Gecko codes load when several share the card (Config.toml [mods] sd_codes = the folder).
        var codesCombo = new ComboBox { Width = 300, Margin = new Thickness(3, 4, 3, 4) };
        var codesRow = Row(new TextBlock { Text = "Build to play", Width = 200, FontWeight = FontWeight.Bold, VerticalAlignment = VerticalAlignment.Center }, codesCombo);
        codesRow.Margin = new Thickness(0, 4, 0, 4);
        body.Children.Add(codesRow);
        bool fillingCodes = false;
        var builds = new List<string>();
        void FillCodes()
        {
            fillingCodes = true;
            builds = SdBuilds();
            var items = new List<string> { "The first build on the card", "Vanilla Brawl (no mods)" };
            items.AddRange(builds);
            codesCombo.ItemsSource = items;
            var chosen = config.Get("mods", "sd_codes") ?? "";
            int i = builds.IndexOf(chosen);
            codesCombo.SelectedIndex = chosen == "vanilla" ? 1 : i < 0 ? 0 : i + 2;
            fillingCodes = false;
        }
        codesCombo.SelectionChanged += (_, _) =>
        {
            if (fillingCodes || codesCombo.SelectedIndex < 0) return;
            if (codesCombo.SelectedIndex == 0) config.Remove("mods", "sd_codes");
            else if (codesCombo.SelectedIndex == 1) config.SetString("mods", "sd_codes", "vanilla");  // no codes, no SD card
            else config.SetString("mods", "sd_codes", builds[codesCombo.SelectedIndex - 2]);
            config.Save();
        };
        FillCodes();
        // Add a mod: the build folder is found inside a .zip or an extracted download and copied onto the card.
        void AddMod(string? source)
        {
            if (string.IsNullOrEmpty(source)) return;
            sdStatus.Text = "Adding the mod...";
            Task.Run(() =>
            {
                try
                {
                    var name = ModInstall.Install(source, Directory.CreateDirectory(sdDir).FullName,
                        n => { if (n % 200 == 0) Dispatcher.UIThread.Post(() => sdStatus.Text = $"Adding the mod... {n} files"); });
                    Dispatcher.UIThread.Post(() =>
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
                    Dispatcher.UIThread.Post(() => sdStatus.Text = ex.Message);
                }
            });
        }
        body.Children.Add(Row(
            Btn("Add a mod from its .zip...", async () => AddMod(await PickFile("Choose the mod's Wii download", "Mod downloads", new[] { "*.zip" }, null))),
            Btn("Add a mod from a folder...", async () => AddMod(await PickFolder("Choose the folder the mod's Wii download was extracted to")))));
        body.Children.Add(Subheading("File and texture mods"));
        body.Children.Add(Note("How to install one:\n" +
                               "1. Click Open mods folder.\n" +
                               "2. Put the mod's folder in it (one folder per mod: files/ mirrors the disc, textures/ holds texture replacements).\n" +
                               "3. Click Refresh. Ticked mods load when the game starts; untick one to turn it off."));
        body.Children.Add(Row(Btn("Open mods folder", () => Open(Directory.CreateDirectory(modsDir).FullName)), Btn("Refresh", () => { LoadMods(); FillCodes(); RefreshStatus(); }, Card)));
        modList = new StackPanel { Margin = new Thickness(8, 4, 8, 4) };
        var list = new StackPanel();
        list.Children.Add(new Border
        {
            Child = new TextBlock { Text = "Installed mods", Margin = new Thickness(8, 4, 8, 4) },
            BorderBrush = EdgeBrush, BorderThickness = new Thickness(0, 0, 0, 1),
        });
        list.Children.Add(new ScrollViewer { Content = modList, Height = 180 });
        body.Children.Add(new Border
        {
            Child = list, Width = 660, Background = new SolidColorBrush(Night), BorderBrush = EdgeBrush, BorderThickness = new Thickness(1),
            Margin = new Thickness(0, 6, 0, 6), HorizontalAlignment = HorizontalAlignment.Left,
        });
        LoadMods();
        return card;
    }

    void LoadMods()
    {
        modList.Children.Clear();
        if (!Directory.Exists(modsDir)) return;
        foreach (var dir in Directory.GetDirectories(modsDir).OrderBy(d => d, StringComparer.Ordinal))
        {
            var box = new CheckBox { Content = Path.GetFileName(dir), IsChecked = !File.Exists(Path.Combine(dir, "disabled")) };
            box.IsCheckedChanged += (_, _) =>
            {
                var marker = Path.Combine(dir, "disabled");
                if (box.IsChecked == true) File.Delete(marker); else File.WriteAllText(marker, "");
                RefreshStatus();
            };
            modList.Children.Add(box);
        }
    }
}

// The launcher's flat button (the Windows launcher's FlatStyle buttons): edge border, bold label, own colour.
sealed class PaperButton : Border
{
    readonly TextBlock label;
    Color back;
    bool hover;
    public Color? HoverColor;
    public event Action? Click;

    public PaperButton(string text, Color back, double fontSize)
    {
        label = new TextBlock { Text = text, FontWeight = FontWeight.Bold, FontSize = fontSize, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center };
        Child = label;
        BorderThickness = new Thickness(2);
        Padding = new Thickness(10, 4, 10, 4);
        Margin = new Thickness(0, 6, 10, 6);
        Cursor = MainWindow.BallHand;
        HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left;  // sized to the label, like the Windows AutoSize buttons
        Back = back;
        PointerEntered += (_, _) => { hover = true; Paint(); };
        PointerExited += (_, _) => { hover = false; Paint(); };
        PointerPressed += (_, e) =>
        {
            if (!IsEnabled || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            e.Handled = true;
            Click?.Invoke();
        };
        PropertyChanged += (_, e) => { if (e.Property == IsEnabledProperty) Opacity = IsEnabled ? 1 : 0.55; };
    }

    public string Text { get => label.Text ?? ""; set { label.Text = value; Child = label; } }

    // Shows a label image (twice its display size) in place of the text; Text keeps the name.
    public void ShowImage(Bitmap? image)
    {
        if (image == null) { Child = label; return; }
        Child = new Image
        {
            Source = image, Width = image.PixelSize.Width / 2.0, Height = image.PixelSize.Height / 2.0,
            VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center, HorizontalAlignment = Avalonia.Layout.HorizontalAlignment.Left,
        };
    }
    public Color Back { get => back; set { back = value; Paint(); } }
    public IBrush? TextBrush { set => label.Foreground = value; }

    // Orange and gold buttons take dark text, the rest light; the nav buttons light up under the pointer.
    void Paint()
    {
        Background = new SolidColorBrush(hover && HoverColor is { } h && back == MainWindow.Card ? h : back);
        BorderBrush = new SolidColorBrush(back == MainWindow.Card ? MainWindow.Edge : back);
        if (label.Foreground == null || label.Foreground == MainWindow.SnowBrush || Equals(label.Foreground, Brushes.Black))
            label.Foreground = back == MainWindow.Orange || back == MainWindow.Gold ? new SolidColorBrush(MainWindow.Night) : MainWindow.SnowBrush;
    }
}
