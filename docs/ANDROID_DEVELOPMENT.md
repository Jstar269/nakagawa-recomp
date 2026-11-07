# Android development

Status: the launcher and the source-owned showcase demos build into one APK for arm64-v8a and
x86_64 and run in the Android emulator. Retail games, importing a disc on the device, and a
supported-device list are in the works ([#360](https://github.com/Jstar269/nakagawa-recomp/issues/360)).

This page covers building and running the Android app from source. The plan it belongs to is in
[`PLATFORM_PORTABILITY.md`](PLATFORM_PORTABILITY.md#3-android).

## What runs where

- **The launcher** is the player (`src/player/`), built as `libmain.so` and started by
  `LauncherActivity` through SDL's Android Java layer.
- **A title** runs in its own Android process, `:title`, as `TitleActivity`. The desktop player
  starts one child process per title; an Android app may not execute programs from its own files
  (API 29 and later) and cannot fork itself safely, so the launcher asks Android to start that
  activity instead, with the same arguments and environment a desktop child gets
  (`nk_platform_spawn_process`, Android section of `src/core/nk_platform_posix.c`). The activity
  loads the title's runtime library, `libnk_title_<id>.so`, runs the runtime's `main()`, and its
  process ends when the run ends, as a desktop child's does. The guest sees no difference.
- **A title's runtime library** is the package's generated C compiled with the runtime for the
  device's ABI (`nk_add_title_runtime` in `CMakeLists.txt`). Packages are built on a PC by the
  ordinary route; the phone does not run the recompiler or a C compiler.

## Prerequisites

Command-line tools from the Android SDK are enough; Android Studio is not needed. The versions
below are the ones this was built and tested with.

- Android SDK with platform `android-37.2`, build-tools 37.0.0, platform-tools, CMake 4.1.2 and
  NDK 30.0.16248370 (`android sdk install` from cmdline-tools 23, or the SDK manager).
- Gradle 9.8.2 (`android/gradle/wrapper/gradle-wrapper.properties` pins it; run
  `gradle -p android wrapper` once to create a local `gradlew`, which stays untracked).
- A JDK 17 or later (Android Studio's bundled JBR works).
- Git, for the pinned SDL3 3.4.20 and SDL3_ttf 3.2.2 sources the build fetches into
  `build/android-deps` (the commits hosted CI builds for Linux; `android/app/deps.cmake`).
- For the showcase demos: PSPDEV, or prebuilt demo PRXs, as for the desktop showcase
  ([`SHOWCASE.md`](SHOWCASE.md)).

Point Gradle at the SDK with `ANDROID_HOME`, or with an `android/local.properties` file
(`sdk.dir=...`), which is ignored and never committed.

## Build

The Gradle project is `android/settings.gradle.kts` and `android/build.gradle.kts`, with the app module in
`android/app/build.gradle.kts`. Its native build, `android/app/CMakeLists.txt`, builds the launcher from the
repository's top-level CMake project and links each bundled title with `nk_add_title_runtime()`. A debug build
merges `android/app/src/debug/AndroidManifest.xml`, which lets `adb shell am start` open `TitleActivity` for
device tests; a release build keeps that activity private.

1. Build the showcase packages on the PC, outside the repository:

   ```bash
   NK_SHOWCASE_BUILD_ROOT=<scratch>/showcase-build \
   NK_SHOWCASE_DEMO_ROOT=<scratch>/showcase-demo \
   python fixtures/showcase/showcase.py build
   ```

2. Build the APK with those packages bundled:

   ```bash
   gradle -p android assembleDebug \
     "-Pnk.titles=showcase-scene-v1=<scratch>/showcase-demo/packages/TEST00007;showcase-breakout-v1=<scratch>/showcase-demo/packages/TEST00008"
   ```

   The APK is `android/app/build/outputs/apk/debug/app-debug.apk`. `-Pnk.abis=arm64-v8a` builds
   one ABI only. Without `nk.titles` the APK carries the launcher alone.

Hosted CI's Android launcher build job builds that launcher-only APK for arm64-v8a from a
clean checkout, with the versions listed under [Prerequisites](#prerequisites), on every change
that runs the native gates ([`CI.md`](CI.md#android-launcher-build)). It builds and packages;
it does not run the APK. It keeps the APK for 14 days as the artifact
`nakagawa-android-arm64-debug`, for [testing on a phone](#test-on-a-phone).

## Run in the emulator

Create an x86_64 Google APIs AVD (API 37.2 was used) and start it headless:

```bash
emulator -avd <name> -no-window -no-audio -gpu swiftshader_indirect
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n io.github.jstar269.nakagawa/.LauncherActivity
```

`swiftshader_indirect` gives the guest Vulkan 1.4 through gfxstream on SwiftShader
(`adb shell cmd gpu vkjson`), with no host GPU. Debug builds let `adb` start a bundled title
directly, as the launcher does:

```bash
adb shell am start -n io.github.jstar269.nakagawa/.TitleActivity \
  --es library nk_title_showcase-scene-v1 \
  --esa args "--image,<app files>/titles/showcase-scene-v1/showcase-scene-v1_image.bin,0x08804000,0x088040ec,none,none,--gui"
adb logcat -s nakagawa nakagawa-title SDL
```

`<app files>` is `/data/user/0/io.github.jstar269.nakagawa/files`. Add
`--esa env "SR_GESTAT=1,SR_EXIT_AT_VBLANK=600"` for the runtime's frame counters and a bounded
run. The runtime's and the player's standard output and error go to the system log under the
tags `nakagawa-title` and `nakagawa`.

`adb install --abi arm64-v8a` installs the arm64 libraries instead, which an x86_64 Google APIs
image runs through its ARM translation layer: a functional check of the arm64 build, not a
measurement of a real device.

## Test on a phone

A physical arm64 phone (Android 10 or later) can run the debug APK. Two routes:

| Route | Needs | Gives |
| --- | --- | --- |
| A. Install the CI build on the phone | A GitHub sign-in on the phone | The launcher only |
| B. Install from the PC with `adb` | A USB cable, and the PC build above for the showcase demos | The launcher, the showcase demos, and a log to attach |

The debug APK is signed with a throwaway key, a different one for every build. To install a
newer APK from another build, uninstall the old one first. Uninstalling deletes the app's
`Nakagawa/` folder, which holds nothing yet that you would miss.

### Route A: install the CI build, no PC

1. On the phone, open the repository's **Actions** tab, workflow **CI**, and the latest run on
   `main` (or on the pull request you are testing). On the run's summary page, under
   **Artifacts**, download `nakagawa-android-arm64-debug`. It downloads as a `.zip`.
2. Open the `.zip` in the **Files** app and extract it. You get `app-debug.apk`.
3. Tap `app-debug.apk`. The first time, Android asks you to allow installs from Files: follow
   **Settings**, turn on **Allow from this source** (under **Apps** > **Special app access** >
   **Install unknown apps**), then go back and install. If Play Protect warns about an unknown
   developer, choose **More details** > **Install anyway**.
4. Open **Nakagawa** from the app drawer.

This build has no titles in it, and the launcher does not list bundled titles yet. It tests the
launcher itself, and a log needs route B.

### Route B: install from the PC with adb

1. On the phone, turn on **Developer options**: **Settings** > **About phone**, tap **Build
   number** seven times. Then, in **Settings** > **System** > **Developer options**, turn on
   **USB debugging**.
2. Connect the phone by USB and accept the debugging prompt on the phone. `adb devices` lists it
   as `device`.
3. Build the APK with the showcase demos bundled ([Build](#build), with `-Pnk.abis=arm64-v8a`),
   or use the CI artifact from route A for the launcher alone. Install it:

   ```bash
   adb install -r android/app/build/outputs/apk/debug/app-debug.apk
   ```

4. Start the launcher from the app drawer. Start a showcase demo with the `adb shell am start`
   command under [Run in the emulator](#run-in-the-emulator). On a phone, `<app files>` is the
   same `/data/user/0/io.github.jstar269.nakagawa/files`.

### What to try

- **Launcher start.** It opens, draws the library screen and responds to touch or a gamepad.
- **A showcase demo** (route B). It starts in landscape and animates, with sound.
- **Rotate** the phone while the demo runs.
- **Fold and unfold** while the demo runs. The screen size changes, so the presenter has to
  recreate its swapchain.
- **Background and return.** Press Home while the demo runs, wait ten seconds, and switch back.
  In the emulator the demo kept running unseen and did not draw again after returning (the
  Lifecycle row below). Report what the phone does.
- The same rotate, fold and background steps on the launcher.

### What to report

For each item: what you did, what you saw (a short screen recording helps), and whether it
recovered. Add the phone model, its Android version (**Settings** > **About phone**), and which
build you installed (the CI run's link, or the commit you built).

With route B, capture the app's own log. Clear the log first, run the steps, then save it:

```bash
adb logcat -c
adb logcat -d -v time -s nakagawa nakagawa-title SDL > nakagawa-android.log
```

The `-s` filter keeps only the launcher's, the title runtime's and SDL's lines, so other apps'
lines and the rest of the system log stay out. Do not attach a full `adb logcat` or a bug report
from Developer options: those carry other apps' data and account details.

## Storage

The player's data, settings, caches, logs and saves are in `Nakagawa/` in the app's private
files folder (`NK_ANDROID_DATA_DIR`, set by the activities), the folder name every host uses. The files the APK bundles (`assets/`
with the UI fonts, `titles/` with the bundled titles) are unpacked into the files folder itself
on the first start after an install or update, and it is the working directory: SDL resolves a
relative path against that folder, so `SDL_GetBasePath()` (`./` on Android) finds them.

## What doesn't yet

| Area | Status |
| --- | --- |
| Launcher library | Bundled titles are not yet listed by the launcher; they start from `adb` (above). |
| Disc import | Picking the user's own ISO through the system document picker (SAF) is not wired. |
| Packages from a PC | Moving a package built on a PC to the phone and running it is not wired. |
| Lifecycle | Measured in the emulator: with the title in the background the guest keeps running and every present fails (`swapchain acquire failed`), because the presenter does not recreate its Vulkan surface and swapchain after Android destroys the window surface. Pausing the guest in the background and recreating the swapchain on return are not done. |
| Input | Touch controls are not built; SDL gamepads reach the runtime's ordinary input path. |
| Video | The FFmpeg H.264 subset builds for Android; it has not been run on a device. |
| Devices | Only the emulator has run it; no physical arm64 device has. Memory and API floors are not yet measured. |

<!-- issue-links: #360 -->
