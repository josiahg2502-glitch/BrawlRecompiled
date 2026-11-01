package com.brawlrecompiled;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Color;
import android.graphics.Typeface;
import android.text.InputType;
import android.util.TypedValue;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * LAN play's devices: this phone's addresses for other players, and the devices the game asks for
 * rooms directly, since Tailscale carries no broadcasts. The game keeps them in Netplay/hosts.txt
 * (netplay_lobby.h) and adds the ones it meets; here the player can add a device's Tailscale name
 * or address by hand, or remove one.
 */
final class DevicesDialog {
    private DevicesDialog() {}

    static File hostsFile(Activity activity) {
        return new File(new File(AppFiles.dataDir(activity), "Netplay"), "hosts.txt");
    }

    static List<String> readDevices(Activity activity) {
        final List<String> devices = new ArrayList<>();
        try (BufferedReader in = new BufferedReader(new InputStreamReader(
                new FileInputStream(hostsFile(activity)), StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (!line.isEmpty() && !line.startsWith("#")) devices.add(line);
            }
        } catch (IOException ignored) {
            // none yet
        }
        return devices;
    }

    static void writeDevices(Activity activity, List<String> devices) {
        final File file = hostsFile(activity);
        //noinspection ResultOfMethodCallIgnored
        file.getParentFile().mkdirs();
        final StringBuilder text = new StringBuilder(
                "# Devices asked for LAN play rooms directly (Tailscale doesn't carry broadcasts).\n"
                + "# One Tailscale name or address per line.\n");
        for (String device : devices) text.append(device).append('\n');
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
            // the list stays as it was
        }
    }

    /** IPv4 addresses of this phone, Tailscale (100.64.0.0/10) first. */
    static List<String> ownAddresses() {
        final List<String> tailscale = new ArrayList<>(), other = new ArrayList<>();
        try {
            for (NetworkInterface itf : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!itf.isUp() || itf.isLoopback()) continue;
                for (InetAddress address : Collections.list(itf.getInetAddresses())) {
                    if (!(address instanceof Inet4Address)) continue;
                    final byte[] b = address.getAddress();
                    final boolean isTailscale = (b[0] & 0xFF) == 100 && (b[1] & 0xC0) == 64;
                    (isTailscale ? tailscale : other).add(address.getHostAddress());
                }
            }
        } catch (Exception ignored) {
            // no network information
        }
        tailscale.addAll(other);
        return tailscale;
    }

    static void show(Activity activity) {
        final float density = activity.getResources().getDisplayMetrics().density;
        final int pad = Math.round(20 * density);
        final LinearLayout column = new LinearLayout(activity);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(pad, pad / 2, pad, 0);

        final StringBuilder own = new StringBuilder();
        for (String address : ownAddresses()) {
            if (own.length() > 0) own.append('\n');
            own.append(address).append(address.startsWith("100.") ? "  (Tailscale)" : "  (Wi-Fi)");
        }
        column.addView(label(activity, "This phone", 17, true));
        column.addView(label(activity, own.length() > 0 ? own.toString() : "No network", 15, false));
        column.addView(label(activity, "Rooms on your Wi-Fi are found by themselves. Over Tailscale, PCs find "
                + "every device, and devices that have played together find each other. To find another "
                + "phone the first time, add its Tailscale name or address here (or it adds yours).", 13, false));

        column.addView(label(activity, "Devices asked for rooms", 17, true));
        final List<String> devices = readDevices(activity);
        if (devices.isEmpty()) {
            column.addView(label(activity, "None yet", 15, false));
        }
        final AlertDialog[] dialog = new AlertDialog[1];
        for (String device : devices) {
            final TextView row = label(activity, device + "   ✕", 15, false);
            row.setPadding(0, Math.round(8 * density), 0, Math.round(8 * density));
            row.setOnClickListener(v -> new AlertDialog.Builder(activity)
                    .setMessage("Stop asking " + device + " for rooms?")
                    .setPositiveButton("Remove", (d, w) -> {
                        final List<String> list = readDevices(activity);
                        list.remove(device);
                        writeDevices(activity, list);
                        dialog[0].dismiss();
                        show(activity);
                    })
                    .setNegativeButton("Cancel", null)
                    .show());
            column.addView(row);
        }

        final ScrollView scroll = new ScrollView(activity);
        scroll.addView(column);
        dialog[0] = new AlertDialog.Builder(activity)
                .setTitle("Tailscale & LAN Devices")
                .setView(scroll)
                .setPositiveButton("Add Device…", (d, w) -> askToAdd(activity))
                .setNegativeButton("Done", null)
                .show();
    }

    private static void askToAdd(Activity activity) {
        final EditText input = new EditText(activity);
        input.setHint("e.g. s26-ultra or 100.84.140.32");
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        input.setSingleLine(true);
        final float density = activity.getResources().getDisplayMetrics().density;
        final LinearLayout box = new LinearLayout(activity);
        final int pad = Math.round(20 * density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(input, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        new AlertDialog.Builder(activity)
                .setTitle("Add a Device")
                .setMessage("Its Tailscale name (as in the Tailscale app) or address. It is asked for rooms "
                        + "from now on.")
                .setView(box)
                .setPositiveButton("Add", (d, w) -> {
                    final String text = input.getText().toString().replaceAll("\\s+", "");
                    final List<String> list = readDevices(activity);
                    if (!text.isEmpty() && !text.startsWith("#") && !list.contains(text)) {
                        list.add(text);
                        writeDevices(activity, list);
                    }
                    show(activity);
                })
                .setNegativeButton("Cancel", (d, w) -> show(activity))
                .show();
    }

    private static TextView label(Activity activity, String text, int sp, boolean bold) {
        final TextView view = new TextView(activity);
        view.setText(text);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        view.setTextColor(bold ? Color.WHITE : Color.rgb(200, 206, 212));
        if (bold) {
            view.setTypeface(Typeface.DEFAULT_BOLD);
            view.setPadding(0, Math.round(14 * activity.getResources().getDisplayMetrics().density), 0, 0);
        }
        return view;
    }
}
