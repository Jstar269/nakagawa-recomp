// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * What the native code needs from Android before it starts, shared by both activities.
 *
 * <p>The native host layer (src/core/nk_platform_posix.c, Android sections) reads three
 * environment variables: NK_ANDROID_DATA_DIR, the app-private folder that holds the player's
 * data, settings, caches, logs and saves; NK_ANDROID_APP_DIR, the folder the APK's bundled files
 * (the "app" asset tree: UI fonts, bundled titles) are unpacked to, which becomes the working
 * directory; and NK_ANDROID_NATIVE_LIB_DIR, where the title runtime libraries are.
 *
 * <p>The bundled files are unpacked into the app's files folder itself: SDL_GetBasePath() is
 * "./" on Android, and SDL opens a relative path against the files folder
 * (SDL_GetAndroidInternalStoragePath), while plain fopen() resolves it against the working
 * directory. With both the same folder, the two agree.
 */
final class NakagawaHost {
    static final String TAG = "nakagawa";

    private NakagawaHost() {}

    static File dataDir(Context context) {
        return new File(context.getFilesDir(), "nakagawa");
    }

    static File appDir(Context context) {
        return context.getFilesDir();
    }

    /** Every variable set here, for {@link #nativeEnvironment}. */
    private static final java.util.LinkedHashMap<String, String> sEnvironment =
        new java.util.LinkedHashMap<>();

    static synchronized void setenv(String name, String value) {
        sEnvironment.put(name, value);
        try {
            Os.setenv(name, value, true);
        } catch (ErrnoException e) {
            Log.e(TAG, "setenv " + name + " failed", e);
        }
    }

    /**
     * The variables set through {@link #setenv}, as KEY=VALUE, read by the native libraries when
     * they load (JNI_OnLoad in src/core/nk_platform_posix.c). Os.setenv changes the environment
     * of the libc the app process runs on; native code that runs on another one, as an arm64
     * library does under a device's ARM translation layer, does not see it, so the native side
     * also takes the values from here.
     */
    static synchronized String[] nativeEnvironment() {
        String[] out = new String[sEnvironment.size()];
        int i = 0;
        for (java.util.Map.Entry<String, String> e : sEnvironment.entrySet()) {
            out[i++] = e.getKey() + "=" + e.getValue();
        }
        return out;
    }

    /** Sets the environment and unpacks the bundled files when this APK version has not yet. */
    static void prepare(Context context) {
        File data = dataDir(context);
        File app = appDir(context);
        if (!data.isDirectory() && !data.mkdirs()) Log.e(TAG, "cannot create " + data);
        setenv("NK_ANDROID_DATA_DIR", data.getAbsolutePath());
        setenv("NK_ANDROID_APP_DIR", app.getAbsolutePath());
        setenv("NK_ANDROID_NATIVE_LIB_DIR", context.getApplicationInfo().nativeLibraryDir);
        String stamp = apkStamp(context);
        File stampFile = new File(app, ".unpacked");
        if (stamp.equals(readText(stampFile))) return;
        try {
            // Replace only what the APK bundles (assets/, titles/), never the user's data.
            String[] bundled = context.getAssets().list("app");
            if (bundled != null) {
                for (String child : bundled) {
                    deleteTree(new File(app, child));
                    copyAssetTree(context.getAssets(), "app/" + child, new File(app, child));
                }
            }
            writeText(stampFile, stamp);
            Log.i(TAG, "BUNDLED_FILES_UNPACKED dir=" + app + " stamp=" + stamp);
        } catch (IOException e) {
            Log.e(TAG, "BUNDLED_FILES_UNPACK_FAILED dir=" + app, e);
        }
    }

    private static String apkStamp(Context context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return info.getLongVersionCode() + "-" + info.lastUpdateTime;
        } catch (Exception e) {
            return "unknown";
        }
    }

    private static void copyAssetTree(AssetManager assets, String path, File target) throws IOException {
        String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                throw new IOException("cannot create " + parent);
            }
            try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[1 << 16];
                int n;
                while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
            }
            return;
        }
        if (!target.isDirectory() && !target.mkdirs()) throw new IOException("cannot create " + target);
        for (String child : children) copyAssetTree(assets, path + "/" + child, new File(target, child));
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        if (file.exists() && !file.delete()) Log.w(TAG, "cannot delete " + file);
    }

    private static String readText(File file) {
        try (InputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[256];
            int n = in.read(bytes);
            return n > 0 ? new String(bytes, 0, n, StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }
}
