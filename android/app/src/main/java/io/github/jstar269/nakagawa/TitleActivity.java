// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * One title's runtime, in the ":title" process. SDL calls nk_android_title_main() in the title's
 * library (android/app/src/main/cpp/nk_android_title.c), which runs the runtime's main() with the
 * launcher's arguments and ends the process when the run ends, as a desktop child process ends.
 *
 * <p>Extras: library (a path in the native library folder, or a bare name such as
 * nk_title_showcase-scene-v1), args (String[]), env (String[] of KEY=VALUE), cwd, log.
 */
public class TitleActivity extends SDLActivity {
    static final String EXTRA_LIBRARY = "library";
    static final String EXTRA_ARGS = "args";
    static final String EXTRA_ENV = "env";
    static final String EXTRA_CWD = "cwd";
    static final String EXTRA_LOG = "log";

    private String mLibraryName = "";
    private String mLibraryPath = "";
    private String[] mArgs = new String[0];

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        NakagawaHost.prepare(this);
        String library = getIntent().getStringExtra(EXTRA_LIBRARY);
        String[] args = getIntent().getStringArrayExtra(EXTRA_ARGS);
        String[] env = getIntent().getStringArrayExtra(EXTRA_ENV);
        String cwd = getIntent().getStringExtra(EXTRA_CWD);
        String log = getIntent().getStringExtra(EXTRA_LOG);
        if (library != null) {
            String name = new File(library).getName();
            if (name.startsWith("lib")) name = name.substring(3);
            if (name.endsWith(".so")) name = name.substring(0, name.length() - 3);
            mLibraryName = name;
            mLibraryPath = new File(getApplicationInfo().nativeLibraryDir, "lib" + name + ".so").getPath();
        }
        if (args != null) mArgs = args;
        if (env != null) {
            for (String entry : env) {
                int eq = entry.indexOf('=');
                if (eq > 0) NakagawaHost.setenv(entry.substring(0, eq), entry.substring(eq + 1));
            }
        }
        // The working directory stays the app's files folder, where SDL also resolves relative
        // paths (NakagawaHost). A desktop child starts in its package folder; the runtime
        // reads nothing relative to it that the arguments and environment do not name.
        if (cwd != null) Log.i(NakagawaHost.TAG, "TITLE_ACTIVITY package folder " + cwd);
        if (log != null) NakagawaHost.setenv("NK_ANDROID_LOG_FILE", log);
        Log.i(NakagawaHost.TAG, "TITLE_ACTIVITY library=" + mLibraryPath + " args=" + mArgs.length);
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        if (mLibraryName.isEmpty()) return new String[] { "SDL3" };
        return new String[] { "SDL3", mLibraryName };
    }

    @Override
    protected String getMainSharedObject() {
        return mLibraryPath;
    }

    @Override
    protected String getMainFunction() {
        return "nk_android_title_main";
    }

    @Override
    protected String[] getArguments() {
        return mArgs;
    }
}
