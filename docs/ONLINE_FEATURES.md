# Online mode and developer mode (design)

Status: step 1 of the implementation order is built (the switches, the wizard step and the developer log panel);
no online feature is. The player is offline today ([`SETUP.md`](SETUP.md#network-use)). This page is the contract the implementation follows. Maintainer
decisions of 2026-10-23: the launcher gets an update check, a reporting system and a news feed, all behind an
**online mode** that the setup wizard asks about, and a **developer mode** that shows a developer log for
diagnosing new issues.

## Principles

- **Asked once, changeable anytime.** The setup wizard asks about online mode and developer mode on one step.
  Both start switched off, and each is a switch in Settings afterwards.
- **One host.** Online mode talks to `nakagawa.jaycast.net` over HTTPS and nothing else. The news site (The
  FreshWire) cross-posts to it, so the player's allowlist stays one host.
- **Nothing is sent on its own.** No telemetry, analytics, crash upload or identifier. The only automatic traffic is
  fetching the update notice and the news feed while online mode is on. Requests carry no cookie and no ID; the
  user agent names the version and operating system only (`NakagawaRecomp/0.0.1 (Windows)`).
- **Game data never leaves the PC.** No game code, game data, decrypted module, package, save, guest memory or
  capture is ever uploaded (`AGENTS.md`, section 5). A report sends only the fields the user sees in its preview.
- **Offline stays complete.** Every feature works without online mode except these three. A network failure is
  shown once, quietly, and never blocks the library or a game.
- **Games stay offline.** Online mode is a launcher feature. The PSP network services a game calls stay fail-closed
  whatever this setting says.

## Setup wizard step: "Online and developer options"

Shown right after Welcome, before the game step, so every first run sees it (it needs no game). Two switches, each
with one plain sentence:

| Switch | Default | Sentence |
| --- | --- | --- |
| Online mode | Off | "Check nakagawa.jaycast.net for updates and news, and let you send reports. Nothing is sent without you." |
| Developer mode | Off | "Show a developer log on screen to help track down problems. Stays on this PC." |

With online mode on, Settings shows its three parts (updates, news, reports), each on by default and each
switchable alone. The step is skippable; skipping keeps both off.

## Update check

- At most once a day while online mode and updates are on, the player fetches
  `https://nakagawa.jaycast.net/updates.json`: the latest version, its date, the release-notes URL and an Ed25519
  signature over those fields. The public key is compiled into the player; the private key is the maintainer's.
- A valid, newer version shows a notice in the library ("v0.0.2 is available") with **Release notes** (opens the
  browser) and **Not now**. An invalid signature, a downgrade or a malformed file is ignored and logged.
- First version: notify and link only. An in-app download-and-install step comes later, with its own signature
  check over the downloaded file and a user confirmation; it never runs unsigned code.

## News

- The library shows a small news strip from `https://nakagawa.jaycast.net/news.json` (title, date, one-line summary,
  link), fetched at most every few hours and cached for offline display.
- Text only: no remote images, HTML or scripts are rendered. **Read** opens the post in the browser.

## Reports

- A **Report** action on a game's title page and on every error card composes a report inside the player: player
  version and build date, operating system, disc ID and title, the furthest stage reached and the named boundary
  (for example `unsupported-import-sceUmdReplaceProhibit`), and the user's own comment.
- The user sees the exact text before sending, can edit the comment and remove any field, and chooses **Send** or
  **Cancel**. With developer mode on, the user may also attach the developer log, shown in the same preview.
- Until the report endpoint exists, **Send** opens the browser on a pre-filled form (forum or issue tracker) instead
  of posting from the player. The endpoint, when built, rate-limits and stores reports without IP logging.
- Never included: game files, saves, memory, screenshots, file paths outside the redaction below.

## Developer mode

- Adds a **Developer log** panel (toggle with F12 or the controller's Select + R): the player's and the runtime's
  log lines, boot events, the named stop boundary and the first missing imports, with copy and save buttons.
- Local only: developer mode sends nothing. Saving or attaching the log replaces the user's profile path and user
  name with placeholders (`%USERPROFILE%`, `<user>`) before display.
- Release builds keep the panel; it adds no performance cost while hidden.

## Policy and code changes this needs

1. `AGENTS.md`, section 5: offline by default becomes "offline until the user turns on online mode; then only the
   update check, news and user-sent reports, only to `nakagawa.jaycast.net`".
2. Until the first network feature ships, `network_mode` in `settings.json` stays `"offline"` (the only value
   `AGENTS.md` section 5 accepts) and the user's choice lives in a separate `online_mode` key, with the per-feature
   switches (`online_updates`, `online_news`, `online_reports`) and `developer_mode` beside it. `network_mode` gains
   `"online"` when step 2 ships, together with the section 5 change in item 1; any unknown value still reads as
   offline.
3. `docs/SETUP.md` "Network use" gains one row per feature when each ships; the egress allowlist test
   (`tools/test_prereq_fetcher.py`) gains the one host and the one module allowed to use it.
4. Tests never touch the internet: a local stub server and fixed signed fixtures cover the client.

## Maintainer tasks

- DNS and hosting for `nakagawa.jaycast.net`, serving `updates.json`, `news.json` and a privacy page.
- Generating and keeping the Ed25519 update-signing key; publishing its public key with the source.

## Implementation order

1. Settings fields, the wizard step and the developer log panel (no network). **Built.**
   - The settings keys are as item 2 below describes.
   - The switches live on Settings > ONLINE & DEVELOPER and on the wizard step "Online and developer options"
     (right after Welcome), both saying that this version has no online features yet. CONTINUE is the step's
     default focus, so Enter never flips a switch; SKIP turns both off.
   - The developer log (F12, or Select + R on a controller, with developer mode on) holds the player's errors,
     launches, setting changes, the game's exit code and the runtime's boot events (phases, the named stop boundary,
     missing imports), redacted as they are added. COPY puts it on the clipboard; SAVE writes `developer-log.txt` in
     the logs folder. Tests: `tests/native/test_player_state.c` (redaction, the ring, the panel, the step) and
     `tests/native/test_player_ui.py`.
2. A small HTTPS client behind one host allowlist (WinHTTP on Windows, which the build-tool download already uses;
   a portable backend on Linux), with the stub-server tests.
3. Update check with signature verification.
4. News strip.
5. Reports, browser hand-off first, then the endpoint.
