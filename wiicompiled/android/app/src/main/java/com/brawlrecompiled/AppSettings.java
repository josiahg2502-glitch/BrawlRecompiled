package com.brawlrecompiled;

import android.content.Context;
import android.content.SharedPreferences;

/** Player settings, kept in one SharedPreferences file shared by the launcher and the game. */
final class AppSettings {
    static final int ASPECT_4_3 = 0;
    static final int ASPECT_16_9 = 1;
    static final int ASPECT_FILL = 2;

    /** GPU driver workarounds: picked from the phone's GPU, or forced (AuroraGpuCompat). */
    static final int GPU_COMPAT_AUTO = 0;
    static final int GPU_COMPAT_ON = 1;
    static final int GPU_COMPAT_OFF = 2;

    /** Render scales offered in the menu; 1x renders 480 lines, whatever the aspect; 0 the screen's own. */
    static final float[] RESOLUTION_SCALES = {0f, 0.5f, 0.75f, 1f, 1.5f, 2f, 3f, 4f};

    private static final String FILE = "settings";
    private final SharedPreferences prefs;

    AppSettings(Context context) {
        // Both processes (launcher and :game) read it. Writes use commit(): the game process is
        // ended right after its menu closes, which would drop a pending apply().
        prefs = context.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    int aspectMode() { return prefs.getInt("aspect_mode", ASPECT_FILL); }
    void setAspectMode(int mode) { prefs.edit().putInt("aspect_mode", mode).commit(); }

    float resolutionScale() { return prefs.getFloat("resolution_scale", 1f); }
    void setResolutionScale(float scale) { prefs.edit().putFloat("resolution_scale", scale).commit(); }

    int gpuCompat() { return prefs.getInt("gpu_compat", GPU_COMPAT_AUTO); }
    void setGpuCompat(int mode) { prefs.edit().putInt("gpu_compat", mode).commit(); }

    boolean showFps() { return prefs.getBoolean("show_fps", false); }
    void setShowFps(boolean show) { prefs.edit().putBoolean("show_fps", show).commit(); }

    boolean frameGraph() { return prefs.getBoolean("frame_graph", false); }
    void setFrameGraph(boolean on) { prefs.edit().putBoolean("frame_graph", on).commit(); }

    boolean touchControls() { return prefs.getBoolean("touch_controls", true); }
    void setTouchControls(boolean on) { prefs.edit().putBoolean("touch_controls", on).commit(); }

    float touchOpacity() { return prefs.getFloat("touch_opacity", 0.6f); }
    void setTouchOpacity(float opacity) { prefs.edit().putFloat("touch_opacity", opacity).commit(); }



    boolean haptics() { return prefs.getBoolean("haptics", true); }
    void setHaptics(boolean on) { prefs.edit().putBoolean("haptics", on).commit(); }

    /** Open the game straight away when the app starts; the in-game menu leads back here. */
    boolean skipStartMenu() { return prefs.getBoolean("skip_start_menu", false); }
    void setSkipStartMenu(boolean on) { prefs.edit().putBoolean("skip_start_menu", on).commit(); }

    /** "2× (960p)": the scale and the number of lines it renders, as the menu shows it. */
    static String resolutionLabel(float scale) {
        if (scale <= 0f) return "Match screen";
        final String factor = scale == Math.rint(scale)
                ? String.valueOf((int) scale) : String.valueOf(scale);
        return factor + "× (" + Math.round(480 * scale) + "p)";
    }
}
