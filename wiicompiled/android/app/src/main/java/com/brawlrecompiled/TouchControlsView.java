package com.brawlrecompiled;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.util.SparseIntArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

/**
 * The on-screen GameCube controller: the control stick under the left thumb, the face buttons laid out like the
 * GameCube's (a big A, B below-left of it, X and Y curving around it) under the right one, L and R on the top
 * corners, Z beside R, and Start in the middle.
 *
 * <p>Every finger is tracked on its own and stays live wherever it slides:
 * <ul>
 * <li>Over the buttons, whatever is under the finger is pressed, so a thumb can roll from A onto B or X.
 * A finger that lands on empty space presses a button as soon as it slides onto one.</li>
 * <li>On the stick, the stick follows the finger anywhere around it, tilted fully at its rim; a finger keeps the
 * stick until it lifts, even past the edge, and a finger that slides onto the stick from elsewhere takes it.</li>
 * <li>The menu button only opens when a press both starts and ends on it, never from a slide.</li>
 * </ul>
 */
final class TouchControlsView extends View {
    /** GameCube PAD bits, as the native side expects them (dolphin/pad.h). */
    static final int Z = 0x0010, R = 0x0020, L = 0x0040;
    static final int A = 0x0100, B = 0x0200, X = 0x0400, Y = 0x0800, START = 0x1000;
    private static final int MENU = 0x10000;  // local: the app's menu, never sent to the game

    private static final int MODE_BUTTONS = 1;  // the finger presses whatever button it is over
    private static final int MODE_STICK = 2;    // the finger steers the stick until lifted
    private static final int MODE_MENU = 3;     // the press started on the menu button

    interface Listener {
        /** The buttons held (PAD bits) and the stick, -1..1 with y up. */
        void onTouchInput(int buttons, float stickX, float stickY);
        void onMenu();
        /** A tap on the game while its menus are up (menu mode), as fractions of the view. */
        void onTap(float x, float y);
    }

    private static final class Button {
        final int bit;
        final String label;
        final int color;
        final float reach;  // hit radius as a multiple of the drawn radius
        float x, y, r;
        Button(int bit, String label, int color, float reach) {
            this.bit = bit; this.label = label; this.color = color; this.reach = reach;
        }
        boolean hit(float px, float py) {
            final float dx = px - x, dy = py - y, radius = r * reach;
            return dx * dx + dy * dy <= radius * radius;
        }
    }

    private static final int WHITE = Color.rgb(255, 255, 255);
    private final Button[] buttons = {
        new Button(A, "A", Color.rgb(60, 200, 150), 1.2f),
        new Button(B, "B", Color.rgb(230, 70, 70), 1.3f),
        new Button(X, "X", WHITE, 1.3f),
        new Button(Y, "Y", WHITE, 1.3f),
        new Button(Z, "Z", Color.rgb(140, 110, 230), 1.25f),
        new Button(L, "L", WHITE, 1.25f),
        new Button(R, "R", WHITE, 1.25f),
        new Button(START, "START", WHITE, 1.3f),
        new Button(MENU, "≡", WHITE, 1.3f),
    };
    private float stickX, stickY, stickR;  // the stick's centre and its travel
    private float knobX, knobY;            // -1..1, y down (screen)
    private int stickPointer = -1;
    private final SparseIntArray pointerModes = new SparseIntArray();  // pointer id -> MODE_*
    private final SparseIntArray pointerBits = new SparseIntArray();   // pointer id -> bits it holds
    private int held;
    private Listener listener;
    private float opacity = 0.6f;
    private boolean controlsVisible = true;
    private final android.util.SparseBooleanArray freeTaps = new android.util.SparseBooleanArray();
    private boolean menuMode;  // the game's tap-driven menus are up: no controller, taps go to the game
    private boolean haptics = true;
    private final android.util.SparseArray<float[]> tapStarts = new android.util.SparseArray<>();
    private final android.util.SparseLongArray tapTimes = new android.util.SparseLongArray();

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    TouchControlsView(Context context) {
        super(context);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        setFocusable(false);
    }

    void setListener(Listener listener) { this.listener = listener; }

    void setOpacity(float opacity) { this.opacity = opacity; invalidate(); }

    void setHaptics(boolean haptics) { this.haptics = haptics; }

    /** Hidden controls still leave the menu button, so the settings stay reachable. */
    void setControlsVisible(boolean visible) {
        controlsVisible = visible;
        release();
        invalidate();
    }

    boolean controlsVisible() { return controlsVisible; }

    /** Menu mode, while the game's tap-driven menus are up: only the menu button, and taps go to the game. */
    void setMenuMode(boolean on) {
        if (menuMode == on) return;
        menuMode = on;
        release();
        invalidate();
    }

    private boolean showsControls() { return controlsVisible && !menuMode; }

    /** Lets go of everything, e.g. when the app loses focus or a menu opens. */
    void release() {
        pointerModes.clear();
        pointerBits.clear();
        stickPointer = -1;
        knobX = knobY = 0;
        update(0, true);
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutControls(w, h);
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        layoutControls(getWidth(), getHeight());
        return super.onApplyWindowInsets(insets);
    }

    private void layoutControls(int w, int h) {
        if (w == 0 || h == 0) return;
        // Keep the thumbs' controls clear of a camera cutout on either short edge.
        float left = 0, right = 0;
        final WindowInsets insets = getRootWindowInsets();
        if (insets != null && insets.getDisplayCutout() != null) {
            left = insets.getDisplayCutout().getSafeInsetLeft();
            right = insets.getDisplayCutout().getSafeInsetRight();
        }
        final float u = h;  // everything scales with the short side
        stickR = 0.17f * u;
        stickX = left + 0.10f * u + stickR;
        stickY = 0.68f * u;
        final float ax = w - right - 0.30f * u, ay = 0.66f * u;
        place(A, ax, ay, 0.105f * u);
        place(B, ax - 0.20f * u, ay + 0.15f * u, 0.065f * u);
        place(X, ax + 0.19f * u, ay - 0.06f * u, 0.07f * u);
        place(Y, ax - 0.05f * u, ay - 0.20f * u, 0.07f * u);
        place(Z, w - right - 0.11f * u, 0.36f * u, 0.06f * u);
        place(L, left + 0.12f * u, 0.12f * u, 0.075f * u);
        place(R, w - right - 0.28f * u, 0.12f * u, 0.075f * u);
        place(START, w / 2f, 0.90f * u, 0.055f * u);
        place(MENU, w - right - 0.08f * u, 0.10f * u, 0.05f * u);
        invalidate();
    }

    private void place(int bit, float x, float y, float r) {
        for (Button b : buttons) {
            if (b.bit == bit) { b.x = x; b.y = y; b.r = r; }
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        final int action = e.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            release();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            final int i = e.getActionIndex();
            tapStarts.put(e.getPointerId(i), new float[] {e.getX(i), e.getY(i)});
            tapTimes.put(e.getPointerId(i), e.getEventTime());
            press(e.getPointerId(i), e.getX(i), e.getY(i));
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); ++i) {
                slide(e.getPointerId(i), e.getX(i), e.getY(i));
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            final int i = e.getActionIndex();
            final int id = e.getPointerId(i);
            final boolean openMenu = pointerModes.get(id, 0) == MODE_MENU && menuAt(e.getX(i), e.getY(i));
            final float[] start = tapStarts.get(id);
            // Taps go to the game on its menus, and anywhere off the controller's buttons otherwise.
            final boolean tappable = menuMode ? pointerModes.get(id, 0) != MODE_MENU : freeTaps.get(id, false);
            if (tappable && start != null && listener != null
                    && Math.hypot(e.getX(i) - start[0], e.getY(i) - start[1]) < 0.06f * getHeight()
                    && e.getEventTime() - tapTimes.get(id) < 800 && getWidth() > 0 && getHeight() > 0) {
                listener.onTap(e.getX(i) / getWidth(), e.getY(i) / getHeight());
            }
            if (id == stickPointer) {
                stickPointer = -1;
                knobX = knobY = 0;
            }
            tapStarts.remove(id);
            tapTimes.delete(id);
            freeTaps.delete(id);
            pointerModes.delete(id);
            pointerBits.delete(id);
            if (openMenu && listener != null) {
                listener.onMenu();
            }
        }
        int combined = 0;
        for (int i = 0; i < pointerBits.size(); ++i) {
            combined |= pointerBits.valueAt(i);
        }
        update(combined, false);
        return true;
    }

    private void press(int id, float x, float y) {
        if (menuAt(x, y)) {
            pointerModes.put(id, MODE_MENU);
            pointerBits.put(id, MENU);
        } else if (onStick(x, y)) {
            takeStick(id, x, y);
        } else {
            pointerModes.put(id, MODE_BUTTONS);
            pointerBits.put(id, buttonsAt(x, y));
            if (buttonsAt(x, y) == 0) freeTaps.put(id, true);
        }
    }

    private void slide(int id, float x, float y) {
        final int mode = pointerModes.get(id, 0);
        if (mode == MODE_STICK) {
            if (id == stickPointer) steer(x, y);
        } else if (mode == MODE_BUTTONS) {
            if (onStick(x, y)) {
                takeStick(id, x, y);
            } else {
                pointerBits.put(id, buttonsAt(x, y));
            }
        } else if (mode == MODE_MENU) {
            pointerBits.put(id, menuAt(x, y) ? MENU : 0);
        }
    }

    private void takeStick(int id, float x, float y) {
        if (stickPointer >= 0 && stickPointer != id) {
            pointerModes.delete(stickPointer);  // the newest finger on the stick steers it
            pointerBits.delete(stickPointer);
        }
        stickPointer = id;
        pointerModes.put(id, MODE_STICK);
        pointerBits.put(id, 0);
        steer(x, y);
    }

    private void steer(float x, float y) {
        float dx = (x - stickX) / stickR, dy = (y - stickY) / stickR;
        final float length = (float) Math.hypot(dx, dy);
        if (length > 1f) { dx /= length; dy /= length; }
        knobX = dx;
        knobY = dy;
    }

    private boolean onStick(float x, float y) {
        if (!showsControls()) return false;
        final float dx = x - stickX, dy = y - stickY, reach = stickR * 1.5f;
        return dx * dx + dy * dy <= reach * reach;
    }

    private boolean menuAt(float x, float y) {
        for (Button b : buttons) {
            if (b.bit == MENU) return b.hit(x, y);
        }
        return false;
    }

    /** Every button under the point; never the menu. */
    private int buttonsAt(float x, float y) {
        if (!showsControls()) return 0;
        int bits = 0;
        for (Button b : buttons) {
            if (b.bit != MENU && b.hit(x, y)) bits |= b.bit;
        }
        return bits;
    }

    private float sentX, sentY;

    private void update(int bits, boolean force) {
        final boolean stickMoved = knobX != sentX || knobY != sentY;
        if (bits == held && !stickMoved && !force) return;
        final int pressed = bits & ~held & ~MENU;
        held = bits;
        sentX = knobX;
        sentY = knobY;
        if (pressed != 0 && haptics) {
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        }
        if (listener != null) {
            listener.onTouchInput(bits & 0xFFFF, knobX, -knobY);
        }
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        final int base = Math.round(255 * opacity);
        if (showsControls()) {
            drawStick(canvas, base);
        }
        for (Button b : buttons) {
            if (!showsControls() && b.bit != MENU) continue;
            final boolean down = (held & b.bit) != 0;
            fill.setColor(Color.argb(down ? base * 3 / 4 : base / 3,
                    Color.red(b.color), Color.green(b.color), Color.blue(b.color)));
            canvas.drawCircle(b.x, b.y, b.r, fill);
            stroke.setStrokeWidth(Math.max(2f, b.r * 0.06f));
            stroke.setColor(Color.argb(base, Color.red(b.color), Color.green(b.color), Color.blue(b.color)));
            canvas.drawCircle(b.x, b.y, b.r, stroke);
            final int ink = down ? 30 : 255;
            text.setColor(Color.argb(Math.min(255, base + 60), ink, ink, ink));
            text.setTextSize(b.label.length() > 2 ? b.r * 0.42f : b.r * 0.9f);
            canvas.drawText(b.label, b.x, b.y - (text.descent() + text.ascent()) / 2, text);
        }
    }

    private void drawStick(Canvas canvas, int base) {
        // The octagonal gate as a ring, and the knob where the stick is.
        fill.setColor(Color.argb(base / 4, 255, 255, 255));
        canvas.drawCircle(stickX, stickY, stickR, fill);
        stroke.setStrokeWidth(Math.max(2f, stickR * 0.03f));
        stroke.setColor(Color.argb(base, 255, 255, 255));
        canvas.drawCircle(stickX, stickY, stickR, stroke);
        final float kx = stickX + knobX * stickR, ky = stickY + knobY * stickR, kr = stickR * 0.45f;
        fill.setColor(Color.argb(stickPointer >= 0 ? base * 3 / 4 : base / 2, 200, 200, 200));
        canvas.drawCircle(kx, ky, kr, fill);
        canvas.drawCircle(kx, ky, kr, stroke);
    }
}
