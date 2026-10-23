# Online mode and developer mode (design)

Status: PLANNED. Nothing in this page is built yet; the player is offline today
([`SETUP.md`](SETUP.md#network-use)). This page is the contract the implementation follows. Maintainer
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

Shown after the fonts step and before Ready. Two switches, each with one plain sentence:

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

## Online play (later)

A separate, later switch, **Online play**, lets games use the PSP's own network services. It is not part of online
mode: online mode lets the launcher talk to one host, while online play lets game code reach the network, so it is
its own explicit choice with its own sentence in Settings.

- **Ad-hoc multiplayer first.** Most PSP multiplayer is local wireless ad-hoc play. Nakagawa implements the PSP ad-hoc
  services for real and carries the traffic to a relay server the project hosts, or directly between devices on the
  user's local network. One implementation covers many titles. Speaking the protocol of PPSSPP's ad-hoc server would
  let Nakagawa players join PPSSPP players; its code may be adapted under the import rules in `AGENTS.md`, section 4.
- **Infrastructure revivals, per title, later.** A game whose official servers are gone can be revived only by
  rebuilding that game's own server protocol, one title at a time, from interoperability study of the game itself.
- **Never:** anything that needs the console vendor's online-account sign-in or DNAS authentication. Those depend on
  the vendor's keys and certificates, which stay out of the project like firmware and decryption.
- **Still fail-closed where unbuilt.** A network service a title calls that Nakagawa does not implement stays a named,
  visible stop, whatever this switch says; no setting makes a refusal permissive.
- **Scope of traffic:** the project's relay host and the user's local network only, each listed in `docs/SETUP.md`
  "Network use" when it ships.
- **Hardware evidence:** ad-hoc behaviour is measured on PSP hardware (two consoles where the test needs a peer)
  before it is claimed, as for every other runtime behaviour.

## Testing online features

- **The default test suite and hosted CI stay offline.** Online code is tested against a stub server on the loopback
  interface with fixed signed fixtures (a valid update notice, a tampered one, a malformed feed, a timeout), and ad-hoc
  play against a local relay with two player instances on loopback.
- **One labelled online smoke check may use the internet,** opt-in only: it runs when the maintainer starts it or on a
  schedule the maintainer sets, talks only to the project's own host, checks the live update notice's signature,
  fetches the news feed and joins the relay. It is never part of the default suite; without network it reports
  `NOT_RUN`, not a failure.
- When the first online feature ships, the `AGENTS.md` section 5 sentence "Tests never touch the internet" becomes:
  "The default test suite and hosted CI never touch the internet; the labelled online smoke check may, opt-in, and
  only against the project's own host."

## Policy and code changes this needs

1. `AGENTS.md`, section 5: offline by default becomes "offline until the user turns on online mode; then only the
   update check, news and user-sent reports, only to `nakagawa.jaycast.net`".
2. `network_mode` in `settings.json` gains the value `"online"`; any unknown value still reads as offline, and the
   per-feature switches are stored beside it.
3. `docs/SETUP.md` "Network use" gains one row per feature when each ships; the egress allowlist test
   (`tools/test_prereq_fetcher.py`) gains the one host and the one module allowed to use it.
4. Tests never touch the internet: a local stub server and fixed signed fixtures cover the client.

## Maintainer tasks

- DNS and hosting for `nakagawa.jaycast.net`, serving `updates.json`, `news.json` and a privacy page.
- Generating and keeping the Ed25519 update-signing key; publishing its public key with the source.

## Implementation order

1. Settings fields, the wizard step and the developer log panel (no network).
2. A small HTTPS client behind one host allowlist (WinHTTP on Windows, which the build-tool download already uses;
   a portable backend on Linux), with the stub-server tests.
3. Update check with signature verification.
4. News strip.
5. Reports, browser hand-off first, then the endpoint.
6. Online play: ad-hoc relay first, then per-title infrastructure revivals.
