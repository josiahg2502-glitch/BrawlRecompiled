package com.brawlrecompiled;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Start screen: play, or import the player's own game the way Dolphin extracts it (the folder, or
 * a zip of it). Until a game is imported the Dolphin steps are shown here; Help repeats them.
 */
public final class LauncherActivity extends Activity {
    private static final int REQUEST_IMPORT = 1;
    private static final int REQUEST_ZIP = 2;
    /** Set by GameActivity.restartForSession: open the game again once its old process is gone. */
    static final String EXTRA_RESTART_GAME = "restartGame";
    /** Set by the game's "Start Menu": show this screen even with Skip Start Menu on. */
    static final String EXTRA_SHOW_START_MENU = "showStartMenu";

    static final String PROJECT_URL = "https://github.com/josiahg2502-glitch/BrawlRecompiled";

    private static final String DOLPHIN_STEPS =
            "Use your own USA Super Smash Bros. Brawl (RSBE01, revision 1). Easiest: in Dolphin on a "
            + "computer, right-click the game, choose Properties, Filesystem, then right-click the disc and "
            + "choose Extract Entire Disc. Copy that folder (or a zip of it) to this phone and tap Import. "
            + "Dolphin shows the revision under Properties > Info.";

    private AppSettings settings;
    private Button skip;

    private TextView status;
    private Button play;
    private View steps;
    private final Handler main = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        settings = new AppSettings(this);
        // Skip Start Menu: straight into the game, unless the game sent the player here or is
        // restarting itself into a LAN session.
        final Intent launch = getIntent();
        final boolean asked = launch != null && (launch.getBooleanExtra(EXTRA_SHOW_START_MENU, false)
                || launch.getBooleanExtra(EXTRA_RESTART_GAME, false));
        if (savedInstanceState == null && !asked && settings.skipStartMenu() && AppFiles.gameReady(this)) {
            startActivity(new Intent(this, GameActivity.class));
            overridePendingTransition(0, 0);
            finish();
            return;
        }
        final int pad = dp(24);
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(pad, dp(12), pad, pad);
        column.setGravity(Gravity.CENTER_HORIZONTAL);

        final LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        final TextView title = text("Brawl Recompiled", 34);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setGravity(Gravity.START);
        header.addView(title, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        header.addView(link("Help", v -> showHelp()));
        column.addView(header, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        final TextView subtitle = text("Super Smash Bros. Brawl, recompiled to run natively", 15);
        subtitle.setGravity(Gravity.START);
        column.addView(subtitle, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        status = text("", 16);
        status.setPadding(0, dp(20), 0, dp(12));
        column.addView(status);

        play = button("Play");
        play.setOnClickListener(v -> startActivity(new Intent(this, GameActivity.class)));
        column.addView(play);

        skip = button("");
        skip.setOnClickListener(v -> {
            settings.setSkipStartMenu(!settings.skipStartMenu());
            updateSkipButton();
        });
        column.addView(skip);
        final TextView skipHelp = text("On: opening the app goes straight into the game. "
                + "In the game, the phone's Back gesture and Start Menu bring you back here.", 13);
        skipHelp.setTextColor(Color.rgb(170, 178, 186));
        skipHelp.setPadding(0, dp(4), 0, 0);
        column.addView(skipHelp);
        updateSkipButton();

        final Button settingsButton = button("Settings");
        settingsButton.setOnClickListener(v -> SettingsMenu.show(this));
        column.addView(settingsButton);

        final Button devices = button("Tailscale & LAN Devices");
        devices.setOnClickListener(v -> DevicesDialog.show(this));
        column.addView(devices);

        // How to get the game files, the same steps KartPad gives, until a game is imported.
        final LinearLayout stepsBox = new LinearLayout(this);
        stepsBox.setOrientation(LinearLayout.VERTICAL);
        stepsBox.setPadding(0, dp(24), 0, 0);
        final TextView stepsTitle = text("Get the game files (once)", 20);
        stepsTitle.setTypeface(Typeface.DEFAULT_BOLD);
        stepsTitle.setGravity(Gravity.START);
        stepsBox.addView(stepsTitle);
        final TextView stepsText = text(
                "1. In Dolphin on a computer, right-click Super Smash Bros. Brawl and choose Properties.\n"
                + "2. Open Filesystem, right-click the disc at the top and choose Extract Entire Disc.\n"
                + "3. Copy that folder (or a zip of it) to this phone.\n"
                + "4. Tap Import from Extracted Game Data Folder… or Import Game Data Zip… below and choose it.\n\n"
                + "Use the USA disc, revision 1 (RSBE01): Dolphin shows the revision under Properties > Info.", 15);
        stepsText.setGravity(Gravity.START);
        stepsText.setPadding(0, dp(6), 0, 0);
        stepsText.setLineSpacing(dp(3), 1f);
        stepsBox.addView(stepsText);
        steps = stepsBox;
        column.addView(stepsBox, new LinearLayout.LayoutParams(Math.min(dp(560),
                getResources().getDisplayMetrics().widthPixels - 2 * pad), LinearLayout.LayoutParams.WRAP_CONTENT));

        final TextView importHelp = text("Import your own Super Smash Bros. Brawl (USA, RSBE01) once. "
                + "Easiest: the folder Dolphin's Extract Entire Disc makes (DATA, or the folder that holds it), "
                + "or a zip of that folder. Your saves are kept.", 13);
        importHelp.setTextColor(Color.rgb(170, 178, 186));
        importHelp.setPadding(0, dp(24), 0, 0);
        column.addView(importHelp);

        final Button importFolder = button("Import from Extracted Game Data Folder…");
        importFolder.setOnClickListener(v -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_IMPORT));
        column.addView(importFolder);
        final Button importZip = button("Import Game Data Zip…");
        importZip.setOnClickListener(v -> pickZip());
        column.addView(importZip);

        final TextView version = text("Brawl Recompiled " + versionText(), 13);
        version.setTextColor(Color.rgb(120, 128, 136));
        version.setPadding(0, dp(24), 0, 0);
        column.addView(version);

        final ScrollView scroll = new ScrollView(this);
        scroll.addView(column);
        setContentView(scroll);
        restartGameIfAsked(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        restartGameIfAsked(intent);
    }

    /** Network play: the game asked to be started again into a LAN session. */
    private void restartGameIfAsked(Intent intent) {
        if (intent == null || !intent.getBooleanExtra(EXTRA_RESTART_GAME, false)) {
            return;
        }
        intent.removeExtra(EXTRA_RESTART_GAME);
        status.setText("Starting the LAN game...");
        main.postDelayed(() -> startActivity(new Intent(this, GameActivity.class)), 700);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    private void updateSkipButton() {
        skip.setText("Skip Start Menu: " + (settings.skipStartMenu() ? "On" : "Off"));
    }

    private void refresh() {
        final boolean present = AppFiles.hasGameData(this);
        final String problem = present ? AppFiles.gameProblem(AppFiles.gameDir(this)) : null;
        if (!present) {
            status.setText("No game data yet. Import the folder Dolphin made, or a zip of it.");
        } else if (problem != null) {
            status.setText("The imported game can't be played. " + problem);
        } else {
            status.setText("Game data is installed and checked.");
        }
        play.setEnabled(present && problem == null);
        steps.setVisibility(present && problem == null ? View.GONE : View.VISIBLE);
    }

    /** Getting Started, laid out like KartPad's. */
    private void showHelp() {
        final LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(24), dp(16), dp(24), dp(16));
        section(column, "1. Import Super Smash Bros. Brawl", DOLPHIN_STEPS);
        section(column, "2. Play", "Tap Play. A Bluetooth or USB controller plays as a GameCube controller; without one, "
                + "the on-screen buttons are player 1's controller. Online play is in the game's own Options (Online).");
        section(column, "Stuck on a step?", "The GitHub page covers the supported game, the steps above "
                + "and common problems.");
        column.addView(link("Setup Guide on GitHub", v -> openWeb(PROJECT_URL + "#getting-the-game-files")));
        final TextView version = text("Brawl Recompiled " + versionText(), 13);
        version.setGravity(Gravity.START);
        version.setTextColor(Color.rgb(170, 178, 186));
        version.setPadding(0, dp(16), 0, 0);
        column.addView(version);
        final ScrollView scroll = new ScrollView(this);
        scroll.addView(column);
        new AlertDialog.Builder(this).setTitle("Getting Started").setView(scroll)
                .setPositiveButton("Done", null).show();
    }

    private void section(LinearLayout column, String title, String body) {
        final TextView heading = text(title, 20);
        heading.setGravity(Gravity.START);
        heading.setTypeface(Typeface.DEFAULT_BOLD);
        column.addView(heading);
        final TextView text = text(body, 15);
        text.setGravity(Gravity.START);
        text.setTextColor(Color.rgb(200, 206, 212));
        text.setPadding(0, dp(6), 0, dp(18));
        column.addView(text);
    }

    private void openWeb(String url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        } catch (ActivityNotFoundException e) {
            new AlertDialog.Builder(this).setMessage(url).setPositiveButton("OK", null).show();
        }
    }

    private String versionText() {
        try {
            final PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName + " · Build " + info.getLongVersionCode();
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    private void pickZip() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_MIME_TYPES,
                new String[] {"application/zip", "application/x-zip-compressed", "application/x-zip"});
        startActivityForResult(intent, REQUEST_ZIP);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        final Uri uri = data.getData();
        if (requestCode == REQUEST_IMPORT) {
            runImport(progress -> copyGame(uri, progress));
        } else if (requestCode == REQUEST_ZIP) {
            runImport(progress -> copyZip(uri, progress));
        }
    }

    // --- import --------------------------------------------------------------------------------

    private interface Progress { void update(int done, int total, String text); }

    private interface ImportJob { String run(Progress progress) throws IOException; }

    private static final class Entry {
        final String id, name;
        final boolean directory;
        Entry(String id, String name, boolean directory) { this.id = id; this.name = name; this.directory = directory; }
    }

    private void runImport(ImportJob job) {
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        final TextView label = text("Looking at the game files…", 14);
        final LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(24), dp(16), dp(24), dp(8));
        box.addView(bar);
        box.addView(label);
        final AlertDialog dialog = new AlertDialog.Builder(this)
                .setTitle("Importing game data").setView(box).setCancelable(false).show();

        new Thread(() -> {
            String result;
            try {
                result = job.run((done, total, text) -> main.post(() -> {
                    bar.setIndeterminate(total <= 0);
                    if (total > 0) {
                        bar.setMax(total);
                        bar.setProgress(done);
                    }
                    label.setText(text);
                }));
            } catch (Exception e) {
                AppFiles.deleteRecursively(stagingDir());
                result = "Import failed: " + e.getMessage() + "\n\nNothing was changed.";
            }
            final String message = result;
            main.post(() -> {
                dialog.dismiss();
                refresh();
                new AlertDialog.Builder(this).setMessage(message).setPositiveButton("OK", null).show();
            });
        }, "game-import").start();
    }

    private File stagingDir() { return new File(AppFiles.dataDir(this), "game.importing"); }

    /** The checked game at `root` (inside the staging folder) becomes the game; saves stay. */
    private String install(File root, int fileCount) throws IOException {
        final File staging = stagingDir();
        final String problem = AppFiles.gameProblem(root);
        if (problem != null) {
            AppFiles.deleteRecursively(staging);
            return problem + "\n\nNothing was changed.";
        }
        final File game = AppFiles.gameDir(this);
        AppFiles.deleteRecursively(game);
        if (!root.renameTo(game)) throw new IOException("cannot move the imported data into place");
        AppFiles.deleteRecursively(staging);
        return "Imported " + fileCount + " files. Tap Play to start.";
    }

    /** A folder from the document picker: copied into the staging folder, checked, then swapped in. */
    private String copyGame(Uri tree, Progress progress) throws IOException {
        final ContentResolver resolver = getContentResolver();
        String rootId = DocumentsContract.getTreeDocumentId(tree);
        if (!looksLikeGame(resolver, tree, rootId)) {
            String nested = null;
            for (Entry e : list(resolver, tree, rootId)) {
                if (e.directory && e.name.equalsIgnoreCase("DATA") && looksLikeGame(resolver, tree, e.id)) nested = e.id;
            }
            if (nested == null) {
                return "That folder is not an extracted game: it needs files/ and sys/ (or a DATA folder holding "
                        + "them), as Dolphin's Extract Entire Disc makes.";
            }
            rootId = nested;
        }
        // Which game and revision, before copying anything.
        final String problem = treeProblem(resolver, tree, rootId);
        if (problem != null) {
            return problem + "\n\nNothing was changed.";
        }
        final List<String[]> files = new ArrayList<>();  // {documentId, relative path}
        collect(resolver, tree, rootId, "", files);

        final File staging = stagingDir();
        AppFiles.deleteRecursively(staging);
        for (int i = 0; i < files.size(); ++i) {
            final File target = new File(staging, files.get(i)[1]);
            final File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            final Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, files.get(i)[0]);
            try (InputStream in = resolver.openInputStream(document); OutputStream out = new FileOutputStream(target)) {
                if (in == null) throw new IOException("cannot read " + files.get(i)[1]);
                AppFiles.copy(in, out);
            }
            progress.update(i + 1, files.size(), (i + 1) + " of " + files.size() + " files");
        }
        return install(staging, files.size());
    }

    /** sys/boot.bin of the picked folder names the game and its revision. */
    private String treeProblem(ContentResolver resolver, Uri tree, String rootId) throws IOException {
        String sysId = null;
        for (Entry e : list(resolver, tree, rootId)) {
            if (e.directory && e.name.equals("sys")) sysId = e.id;
        }
        String bootId = null;
        if (sysId != null) {
            for (Entry e : list(resolver, tree, sysId)) {
                if (!e.directory && e.name.equals("boot.bin")) bootId = e.id;
            }
        }
        if (bootId == null) {
            return "That folder has no sys/boot.bin, so the game can't be identified.";
        }
        final byte[] header = new byte[8];
        try (InputStream in = resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, bootId))) {
            if (in == null || AppFiles.readFully(in, header) < header.length) {
                return "That folder's sys/boot.bin is too short, so the game can't be identified.";
            }
        }
        return AppFiles.headerProblem(header);
    }

    /** A zip of the folder Dolphin extracted: unpacked into the staging folder, checked, swapped in. */
    private String copyZip(Uri zip, Progress progress) throws IOException {
        final long size = documentSize(zip);
        final File staging = stagingDir();
        AppFiles.deleteRecursively(staging);
        if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
        final String stagingPath = staging.getCanonicalPath() + File.separator;
        int count = 0;
        try (InputStream raw = getContentResolver().openInputStream(zip)) {
            if (raw == null) throw new IOException("cannot read the zip");
            final CountingInputStream counted = new CountingInputStream(new BufferedInputStream(raw, 1 << 16));
            try (ZipInputStream in = new ZipInputStream(counted)) {
                ZipEntry entry;
                while ((entry = in.getNextEntry()) != null) {
                    // Windows PowerShell's zips separate (and end folder entries) with backslashes.
                    final String name = entry.getName().replace('\\', '/');
                    final File target = new File(staging, name);
                    if (!target.getCanonicalPath().startsWith(stagingPath)) {
                        throw new IOException("the zip holds a path outside its folder: " + name);
                    }
                    if (entry.isDirectory() || name.endsWith("/")) {
                        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("cannot create " + target);
                        continue;
                    }
                    final File parent = target.getParentFile();
                    if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
                    try (OutputStream out = new FileOutputStream(target)) {
                        AppFiles.copy(in, out);
                    }
                    ++count;
                    if (size > 0) {
                        final int permille = (int) Math.min(1000, counted.count * 1000 / size);
                        progress.update(permille, 1000, String.format(Locale.ROOT, "%d of %d MB",
                                counted.count >> 20, size >> 20));
                    } else {
                        progress.update(0, 0, count + " files");
                    }
                }
            }
        }
        final File root = findGameRoot(staging, 3);
        if (root == null) {
            AppFiles.deleteRecursively(staging);
            return "That zip doesn't hold an extracted game: zip the folder Dolphin's Extract Entire Disc made "
                    + "(the one with files and sys in it).\n\nNothing was changed.";
        }
        return install(root, count);
    }

    /** The extracted game inside an unpacked zip: its top, DATA, or a folder holding either. */
    private static File findGameRoot(File dir, int depth) {
        if (AppFiles.isGameRoot(dir)) return dir;
        if (depth == 0) return null;
        final File[] children = dir.listFiles(File::isDirectory);
        if (children == null) return null;
        for (File child : children) {
            if (child.getName().equals("__MACOSX")) continue;
            final File found = findGameRoot(child, depth - 1);
            if (found != null) return found;
        }
        return null;
    }

    private long documentSize(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (RuntimeException ignored) {
            // no size: progress counts files instead
        }
        return -1;
    }

    private static final class CountingInputStream extends FilterInputStream {
        long count;
        CountingInputStream(InputStream in) { super(in); }
        @Override public int read() throws IOException {
            final int b = super.read();
            if (b >= 0) ++count;
            return b;
        }
        @Override public int read(byte[] buffer, int offset, int length) throws IOException {
            final int read = super.read(buffer, offset, length);
            if (read > 0) count += read;
            return read;
        }
        @Override public long skip(long n) throws IOException {
            final long skipped = super.skip(n);
            count += skipped;
            return skipped;
        }
    }

    private boolean looksLikeGame(ContentResolver resolver, Uri tree, String id) {
        boolean sys = false, filesDir = false;
        for (Entry e : list(resolver, tree, id)) {
            if (e.directory && e.name.equals("sys")) sys = true;
            if (e.directory && e.name.equals("files")) filesDir = true;
        }
        return sys && filesDir;
    }

    private void collect(ContentResolver resolver, Uri tree, String id, String prefix, List<String[]> out) {
        for (Entry e : list(resolver, tree, id)) {
            final String path = prefix.isEmpty() ? e.name : prefix + "/" + e.name;
            if (e.directory) collect(resolver, tree, e.id, path, out);
            else out.add(new String[] {e.id, path});
        }
    }

    private List<Entry> list(ContentResolver resolver, Uri tree, String id) {
        final List<Entry> entries = new ArrayList<>();
        final Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        final String[] columns = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
        };
        try (Cursor c = resolver.query(children, columns, null, null, null)) {
            while (c != null && c.moveToNext()) {
                entries.add(new Entry(c.getString(0), c.getString(1),
                        DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))));
            }
        }
        return entries;
    }

    // --- views ---------------------------------------------------------------------------------

    private TextView text(String value, int sp) {
        final TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        view.setTextColor(Color.WHITE);
        view.setGravity(Gravity.CENTER_HORIZONTAL);
        return view;
    }

    private TextView link(String label, View.OnClickListener onClick) {
        final TextView view = text(label, 16);
        view.setTextColor(Color.rgb(255, 138, 128));
        view.setTypeface(Typeface.DEFAULT_BOLD);
        view.setPadding(dp(12), dp(10), dp(12), dp(10));
        view.setOnClickListener(onClick);
        view.setBackgroundResource(android.R.drawable.list_selector_background);
        return view;
    }

    private Button button(String label) {
        final Button view = new Button(this);
        view.setText(label);
        view.setAllCaps(false);
        final LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(dp(320), LinearLayout.LayoutParams.WRAP_CONTENT);
        params.topMargin = dp(8);
        view.setLayoutParams(params);
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
