// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

package io.github.jstar269.nakagawa;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * The launcher: SDL runs the player's main() from libmain.so (src/player/main.c) on its thread.
 * When the player starts a title, the native host layer calls {@link #startTitle}.
 */
public class LauncherActivity extends SDLActivity {
    private static volatile LauncherActivity sCurrent;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sCurrent = this;
        NakagawaHost.prepare(this);
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onDestroy() {
        if (sCurrent == this) sCurrent = null;
        super.onDestroy();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected String getMainFunction() {
        return "main";
    }

    @Override
    protected String[] getArguments() {
        return new String[0];
    }

    /**
     * Starts one title's runtime in the ":title" process (TitleActivity). Called from native code
     * (nk_platform_spawn_process) with what a desktop launch passes to its child: the runtime's
     * library path in place of the executable, the arguments after argv[0], KEY=VALUE environment
     * entries, the working directory and the run-log path (either may be null).
     */
    public static boolean startTitle(String library, String[] args, String[] env, String cwd,
                                     String logPath) {
        LauncherActivity launcher = sCurrent;
        if (launcher == null) return false;
        Intent intent = new Intent(launcher, TitleActivity.class);
        intent.putExtra(TitleActivity.EXTRA_LIBRARY, library);
        intent.putExtra(TitleActivity.EXTRA_ARGS, args);
        intent.putExtra(TitleActivity.EXTRA_ENV, env);
        if (cwd != null) intent.putExtra(TitleActivity.EXTRA_CWD, cwd);
        if (logPath != null) intent.putExtra(TitleActivity.EXTRA_LOG, logPath);
        Log.i(NakagawaHost.TAG, "TITLE_START library=" + library + " args=" + args.length);
        launcher.runOnUiThread(() -> launcher.startActivity(intent));
        return true;
    }
}
