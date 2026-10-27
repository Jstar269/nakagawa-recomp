# Accessibility

Nakagawa Recomp turns a PSP game into a native Windows program you play with the project's own player. We want
people with disabilities, and people who use assistive technology, to be able to set the player up, use it and
contribute to it. This page says what is true today, including what is missing, and how to tell us about a barrier.

It describes the state of the `main` branch. No release has been published yet, so there is no release to quote.
The code and tests are the authority: where this page and the code disagree, the code is current, and a report that
says so is welcome.

The games you play are made by other people. This page covers the player, its setup wizard, its settings and its
documentation, not what a game itself shows or plays.

## What we aim for

The launcher (the library, the setup wizard, Settings, Controller settings, the error screens and About) follows a
written design system, [`docs/PLAYER_UI_DESIGN.md`](docs/PLAYER_UI_DESIGN.md). Its accessibility rules are:

- **Controller first, keyboard and mouse equal.** Every launcher action is reachable with a D-pad (or arrow keys) and
  two buttons, focus is always visible, and nothing needs a pointer or hover.
- **Colour never carries meaning alone.** A game's status is a word and a shape (`●` ready, `▲` needs setup,
  `◆` experimental, `?` untested), then a colour.
- **Contrast.** Text is at least 4.5:1 against its background, except text marked disabled and fine print, which the
  design reserves for non-essential use.
- **Targets.** Buttons and rows are at least 44 pixels high.
- **Nothing needs motion to be understood.**
- **Plain words.** Messages say what happened and what to do, and the player names what it cannot do instead of
  pretending.

These numbers come from the design record and the layout checks in `fixtures/nk_ui/`. We have not run an
independent audit, and we do not claim conformance with WCAG or any other standard. WCAG is written for web content,
so we use its contrast and target-size values as a guide for the launcher only.

## What works today

### Input

- **Keyboard and mouse** in the launcher: arrow keys move, Enter selects, Esc goes back, Space opens a screen's
  extra options, `Q` and `E` switch tabs, Tab and Shift+Tab move focus in order, `O` adds a disc image, `S` opens Settings, `F11` or Alt+Enter
  toggles full screen. The mouse works everywhere but is never required.
- **Controllers** the operating system exposes through SDL3 (Xbox, PlayStation and Switch-style pads): D-pad to move,
  the bottom face button to select, the right face button to go back, shoulder buttons to switch tabs.
- **Prompts match what you are using.** The prompt bar at the bottom of each screen shows keyboard keycaps, or the
  button glyphs of your controller as it reports them (letters or shapes). It switches the moment a pad connects or
  you press a key, and falls back when the pad disconnects. The top bar names the active set.
- **Controller remapping for games.** *Settings > Controller settings* lets you rebind every PSP button and the
  analog stick to a primary and a secondary gamepad input, change the dead zones and trigger threshold, and
  calibrate the stick. A rebinding that collides with another is reported.
- The setup wizard works with keyboard, mouse or controller alone, and its default focus is the safe action (Enter
  on the first screen never flips a switch).
- Game text-entry prompts (the PSP on-screen keyboard) open a standard Windows dialog with a text box on Windows,
  so the system's own text input and assistive tools apply to it.

### Seeing the launcher

- **One dark theme** with a near-black ground and light text. Secondary text is 7.6:1 against the ground; the focus
  ring is 15.3:1.
- **Fonts** are the project's own open fonts (Geist, Geist Mono, Chakra Petch, with Noto Sans JP as a fallback,
  all SIL Open Font License). Body text is 15 pixels at 100% scale. The Latin and Japanese scripts are covered;
  other scripts may show as missing glyphs.
- **The window resizes.** The layout reflows by width: the grid goes from one to six columns, margins narrow below
  900 pixels, and there is a minimum window size.
- **Display scale** follows your operating system's scale for the window automatically, so text and targets grow with
  Windows' "Scale" setting.
- **Reduce motion.** *Settings > Reduce motion* freezes the animated background waves and turns pulses and
  sweeps into cuts. It is saved as `reduce_motion` in the player's `settings.json`.
- **Volume** has its own setting (*Settings > Game volume*), separate from any volume setting inside a game.
- **Status and errors are words first.** Every error screen has a plain-language title, a "what to do" sentence and
  a named recovery button. A disc the player cannot use is explained, not just refused.

### Documentation

- The documentation is Markdown, read on GitHub or in any editor. Headings are in order, links have descriptive
  text, and images in the README have alternative text. The Markdown is linted in CI (`markdownlint`).
- Steps are written as numbered lists, with exact commands in code blocks.

## Known limitations

We know about each of these. They are open work, not a promise of dates. Each is checked against the source of
`main`.

| Limitation | What it means for you | Workaround |
| --- | --- | --- |
| **No screen-reader support.** The launcher draws its own interface with SDL3 and Vulkan and does not expose an accessibility tree to NVDA, JAWS or Narrator. | A screen reader cannot read the library, wizard, settings or error screens. | None inside the player. The documentation is plain Markdown. Setup can be done from the command line: see [`docs/SETUP.md`](docs/SETUP.md) and `python tools/nk_cli.py`. |
| **No user control of text size, UI scale or contrast.** The design system can scale the layout 90% to 150% for text and 100% to 200% for the UI, and has a high-contrast focus ring. The player does not expose these yet: text size is fixed at 100%, scale follows the display, and the focus ring is the standard one. | You cannot make launcher text larger than your operating system's display scale makes it. | Raise Windows' display scale or resolution. |
| **The player ignores system preferences** for high contrast, reduced motion and text size. | Windows' own settings for these do not change the player. | Turn on *Reduce motion* in the player's Settings. |
| **No light theme and no colour-blind palette.** | The launcher is always the dark theme. Status is already word-and-shape, so meaning is kept, but the accent and status colours are fixed. | None yet. |
| **Launcher shortcuts are fixed.** The in-game keys mirror a PSP by default (`X`, `Z`, `A`, `S`, `Q`, `W`, Enter, Shift, arrow keys; see [`docs/SETUP.md`](docs/SETUP.md)), and you can change them on the Controls page in Settings (choose **KEYBOARD**, [#1467](https://github.com/Jstar269/nakagawa-recomp/pull/1467)). The launcher's own keys, and Esc, F1 and F3 in a game, can't be changed. | Moving around the launcher with other keys needs a controller or an external remapping tool. | Use a controller (any SDL3 pad), or a system-level key remapper. |
| **No captions, subtitles or transcripts.** The player shows whatever a game shows. Spoken or musical content in a game is not captioned. | If a game relies on sound for information, the player adds nothing to replace it. | None. |
| **No photosensitivity protection.** The player does not limit flashing or motion in a running game. *Reduce motion* affects the launcher only. | A game that flashes will flash. | Check the game's own content warnings before playing. |
| **Launcher text is English only.** | No translation. | None yet. |
| **No accessibility scan in CI.** The layout checks catch focus, spacing and alignment regressions but not every barrier. | A new barrier can land unnoticed. | Report it (below). |
| **No testing with assistive technology and no user research.** We have not tested with screen readers, magnifiers, switch devices or voice control, and we have not worked with disabled users. | Unknown barriers exist. | Report what you find. |

## Supported environments

| Environment | Status |
| --- | --- |
| Windows 11 x64 with a Vulkan-capable GPU, SDL3 | Supported: this is where the player is built and used today. |
| Linux (Ubuntu) | The runtime and the installed player are built and smoke-tested in hosted CI without a display. Interactive use of the launcher on Linux is not supported yet ([`docs/LINUX_DEVELOPMENT.md`](docs/LINUX_DEVELOPMENT.md)). |
| Android | Planned ([`docs/PLATFORM_PORTABILITY.md`](docs/PLATFORM_PORTABILITY.md)). Nothing runs there. |
| macOS and consoles | Not planned for the player. |
| Input | Keyboard and mouse; SDL3 gamepads. Touch, eye tracking, switch and voice devices are untested. |
| Assistive technology | None tested. A tool that works at the operating-system level (a magnifier, a key remapper, an on-screen keyboard) may help, but we have not verified any. |

## Contributing accessibly

If you change the launcher, please:

1. Compose screens from the existing components in `src/ui/` and the design tokens in
   [`docs/PLAYER_UI_DESIGN.md`](docs/PLAYER_UI_DESIGN.md); do not pick raw colours or sizes.
2. Keep every action reachable with the D-pad and two buttons, keep focus visible, and register new controls with the
   focus system.
3. Never use colour as the only signal: add a word or a shape.
4. Keep targets at least 44 pixels high and text at least 4.5:1, except text the design marks disabled.
5. Make motion optional: honour *Reduce motion* and never require an animation to be understood.
6. Run `mingw32-make nk-ui-player-screens-selftest`, and say in the pull request why any golden file changed.
7. Look at the screen with the keyboard alone, then with a controller alone, before you ask for review.

If you change documentation: keep heading levels in order, give links descriptive text, give images alternative
text, put text in text rather than in an image, and do not rely on colour. If you add a chart or diagram, put the
same information in a table or sentence next to it.

If your change adds, changes or removes a barrier, update the tables above in the same pull request.

## Report an accessibility barrier

Open an issue with the
[**Accessibility barrier** form](https://github.com/Jstar269/nakagawa-recomp/issues/new?template=accessibility.yml).
Its title starts with `[Accessibility]:`. Please keep accessibility reports separate from general bugs so they can be
triaged for what they block.

It helps if you tell us:

- what you were trying to do, and where it failed;
- the screen or step (library, setup wizard, settings, in-game window, documentation);
- how you were using the player: keyboard, mouse, controller (which), screen magnifier, other tools;
- your Windows version, display size and scale, and the player's revision;
- how bad it is (below), and any workaround you found.

You do not need to diagnose the cause, or to know any technical term. If a form is itself a barrier, post in the
[Help](https://github.com/Jstar269/nakagawa-recomp/discussions/categories/help) category of Discussions or comment on
any open issue, or use [GitHub private reporting](https://github.com/Jstar269/nakagawa-recomp/security/advisories/new)
if the report contains something private, and say it is an accessibility report. Do not attach game files, private
paths or personal data.

Severity, as the form asks for it:

- **Critical:** you cannot set up or start the player at all.
- **Serious:** you can finish the task only with great difficulty or outside help.
- **Moderate:** you can finish the task, but it is hard or slow.
- **Minor:** a nuisance or an inconsistency.

### What happens next

This is a small project run by one maintainer, so we promise honesty rather than deadlines. We will read every
report, label it, say whether we can reproduce it, say what the next step is, and name a workaround if we know one.
Barriers that block setup or use come first. We do not set fixed response times. If we cannot fix something, we
say so on the issue and add it to the table above.

## Ownership

The maintainer ([@Jstar269](https://github.com/Jstar269)) owns this statement, triages reports, and keeps it current
with the code. Other questions go through [`SUPPORT.md`](SUPPORT.md).

## Feedback on this page

If this statement is wrong, unclear or missing something you need, open an issue or a pull request. Corrections to
what the code actually does are especially welcome.
