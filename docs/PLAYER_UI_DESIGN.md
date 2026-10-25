# Player UI design system

The visual spec for the native player's launcher UI (the default since C10; `--ui=legacy` keeps the old one for
one release). It is the reference every
screen lane builds against: Library and Title detail (C5, built), Settings (C6), first run (C7) and error dialogs
(C8). The tokens and components it names live in code:

| Layer | File | Holds |
| --- | --- | --- |
| Tokens | `src/ui/nk_ui_theme.h`, `src/ui/nk_ui_theme.c` | colours, spacing, radii, borders, control sizes, layout grid, motion, font slots, type scale |
| Components | `src/ui/nk_ui_widgets.h`, `src/ui/nk_ui_widgets.c` | button, game card, add card, hero, status pill, tag, badge, eyebrow, card, divider, top bar, prompt bar, vector art |
| Core | `src/ui/nk_ui.h`, `src/ui/nk_ui.c` | layout (Clay), text runs, the focus ring, frame text storage, custom elements |
| Screens | `src/player/ui/scr_*.c` | compose components; never pick raw colours or sizes |

It follows the owner's interactive prototype (version 8) and the "Player" boards of the design canvas, both private.
Where this page and the code disagree, the code is current and this page is updated in the same change.

## Principles

- **One dark theme.** Ground `#0B0F14`; every surface steps up in colour, not with shadows.
- **Controller first.** Every action is reachable with a D-pad and two buttons, focus is always visible, and the
  prompt bar names the buttons that work on this screen. Nothing needs a pointer or hover.
- **Status is a word and a shape, then a colour:** `●` Ready, `▲` needs setup, `◆` experimental, `?` untested,
  `✕` unsupported. Colour never carries meaning alone.
- **Honest copy.** Words describe what the build does today. Compatibility words come from data
  (`docs/COMPATIBILITY.md`), never from layout code.
- **Bring your own game.** No game data, art, music or fonts ship with the player; placeholders say where the
  user's own disc art will appear.
- **Say it once, where the user decides.** The privacy promise ("Your games stay on this PC. Nakagawa never
  uploads anything, and downloads only the build tools you approve.") appears on the setup wizard's Welcome step
  and on About, and nowhere else: not in prompt-bar corners, not on the Add card, not in page footers. A screen
  that asks for consent (the build-tool download) says exactly what it fetches, from where, and how big it is.
- **No console trade dress.** No PSP system icons, sounds or names, no XMB-style horizontal category bar. The
  waves, the palette and the seasonal arc are the project's own.
- **No key-file UI anywhere.** The player does not decrypt games (#1187). No screen, step, setting, dialog or path
  row asks for, finds or names a key file. A disc whose program is encrypted gets the plain explanation the host
  already supplies ("Nakagawa does not decrypt games; it uses decrypted files from your own copy").

## Tokens

### Colour

| Token (`NkUiTheme`) | Value | Use |
| --- | --- | --- |
| `surface` | `#0B0F14` | window ground, add card |
| `surface_raised` | `#121821` | cards, panels, prompt bar |
| `surface_high` | `#18202B` | chips, focused secondary button, focused row |
| `line` | `#1C232D` | hairlines, card edges |
| `line_strong` | `#2E3846` | control and chip borders, focused card edge |
| `paper` | `#F5F7FA` | primary text |
| `muted` | `#9AA4B2` | secondary text (7.6:1 on the ground) |
| `dim` | `#6B7684` | disabled text and footnotes only (4.2:1, not for body text) |
| `accent_dark` | `#D7F24A` | focus ring, primary button, active tab rule, toggles, sliders, progress |
| `ink` | `#10151C` | text on an accent fill (14.6:1) |
| `accent_pressed` | `#B4D134` | pressed primary button |
| `pressed` / `pressed_danger` | `#222C39` / `#2A1A1C` | pressed secondary / danger button |
| `ready` | `#2DD4BF` | Ready; never the accent |
| `warning` | `#DBB024` | needs setup |
| `info` | `#6CB6FF` | experimental, untested, informational notes |
| `error` | `#FF7B72` | unsupported, failed, remove |
| `scrim` | ground at 78% | behind dialogs and the in-game overlay |
| `art_scrim` | ground at 72% | under a badge on title art |
| `art_label` | `#C9D6E3` | captions over title art |
| `placeholder` | `#3A4552` | art placeholder outline, inactive glyph dots |
| `capture` | `#1F2613` | a row waiting for a button press (C6 controls) |
| `accent_light` | `#6E9600` | accent on a light surface (reserved; the UI has none) |

### Spacing, radii, borders

- Spacing is a 4 px scale: `NK_UI_SPACE_1` 4, `_2` 8, `_3` 12, `_4` 16, `_5` 20, `_6` 24, `_7` 32, `_8` 48, `_9` 64.
- Radii: chip 4 (pills, badges), control 8 (buttons, rows), tile 12 (cards, panels, hero), dialog 16, pill (full).
- Borders are 1 px hairlines (`NK_UI_BORDER_HAIRLINE`). Rounded borders draw straight runs plus antialiased quarter
  rings; every edge rounds to whole device pixels at every UI scale.
- Elevation is a colour step plus an edge: 0 page; 1 card (`surface_raised` + `line`); 2 control or focused row
  (`surface_high` + `line_strong`); 3 dialog (`surface_raised` + `line_strong` over the scrim).

### Type

Fonts are SIL OFL 1.1 files in `assets/fonts/ui/`, registered in eight slots (`NK_UI_FONT_*`); every face falls
back to Geist and then Noto Sans JP. Sizes are logical px; tracking is CSS letter-spacing (it follows every
character).

| Role (`NK_UI_TYPE_*`) | Face | Size / line | Tracking | Use |
| --- | --- | --- | --- | --- |
| `DISPLAY_XL` | Chakra Petch Bold, caps | 40 / 44 | 0.04em | first-run headline |
| `DISPLAY` | Chakra Petch Bold, caps | 30 / 32 | 0.04em | screen names (LIBRARY, SETTINGS) |
| `WORDMARK` | Chakra Petch Bold, caps | 18 / 20 | 0.06em | NAKAGAWA RECOMP in the top bar |
| `TITLE_XL` | Geist SemiBold | 30 / 34 | 0 | a game's name on its page |
| `TITLE` | Geist SemiBold | 22 / 28 | 0 | dialog and panel titles |
| `HEADING` | Geist SemiBold | 17 / 24 | 0 | card headings |
| `TILE_TITLE` | Geist SemiBold | 15 / 20 | 0 | game card names (two lines, then an ellipsis) |
| `BODY_L` | Geist | 17 / 26 | 0 | lead paragraphs |
| `BODY` | Geist | 15 / 22 | 0 | running text |
| `BODY_S` | Geist | 14 / 21 | 0 | card paragraphs |
| `CAPTION` | Geist | 13 / 19 | 0 | hints, counts, notes |
| `FOOTNOTE` | Geist, `dim` | 12 / 16 | 0 | sources and fine print |
| `LABEL` | Geist Mono Medium, caps | 12 / 16 | 0.08em | buttons, prompts, tabs |
| `LABEL_L` | Geist Mono Medium, caps | 14 / 18 | 0.12em | the large primary button |
| `LABEL_S` | Geist Mono Medium, caps | 11 / 14 | 0.06em | chips, card meta rows |
| `EYEBROW` | Geist Mono Medium, caps, `muted` | 11 / 14 | 0.12em | section headers |
| `MICRO` | Geist Mono Medium, caps | 10 / 12 | 0.08em | badges over art |
| `DATA` | Geist Mono | 12 / 16 | 0 | disc IDs, figures, paths |

Labels, chips and eyebrows are drawn in caps (`nk_ui_caps`); the words stay sentence case in data.

### Focus

- The ring is drawn by the core outside the focused element, so nothing moves when focus moves: 3 px
  `accent_dark`, 3 px gap, radius = the element's radius + 6.
- High-contrast focus (`NkUiPrefs.high_contrast_focus`): a 4 px white ring between 2 px black keylines.
- A focusable control calls `nk_ui_register_button` and, when focused, `nk_ui_focus_ring(ui, id, radius)`. A focused
  secondary button also fills with `surface_high`; a focused card's edge becomes `line_strong`.

### Layout grid

| Measure | Value |
| --- | --- |
| Top bar / prompt bar | 64 / 56 px, separated from the body by a `line` hairline |
| Page gutter | 32 px; 16 px below 900 px wide (`NK_UI_GUTTER`, `NK_UI_GUTTER_NARROW`) |
| Body padding | 24 px top, 16 px bottom; 16 px between blocks |
| Library grid | columns = how many 236 px cards fit with 20 px gaps (1 to 6); rows = how many fit; 16 px row gap |
| Game card | art panel 0.53 x width, body 90 px |
| Title detail | 520 px aside (at most 45% of the body) and a growing column, 32 px apart; hero at 480:272 |
| Minimum target | 44 px high (`NK_UI_CONTROL_HEIGHT`); large 52, small 34 |
| Safe area | nothing closer than 16 px to a window edge |

Layout is logical px; the UI scale (Auto follows the display, or 100/125/150/200%) times the text size gives the
device density, and the logical width decides the columns. Clay clips at most ten containers per layout: shorten
long text with `nk_ui_text_clamp` instead of clipping it.

### Motion

Tokens are in seconds (`NK_UI_MOTION_*`): fast 0.12 (focus, press), base 0.20 (toast, tab rule), slow 0.50 (art
crossfade); art appears after a 0.6 s rest, preview music after 1.1 s, fading in over 1.0 s and out over 0.45 s.
The waves step at 30 Hz. Reduce motion (the setting or the system's) makes every duration a cut and holds the
waves on one frame. Nothing ever needs motion to be understood.

### Background waves

Four project-authored ribbons drift behind every launcher screen (`nk_ui_waves_element`, drawn first and outside
the layout), never in game. Their combined alpha is capped so every text colour keeps WCAG AA over the brightest
point. The SDL_Renderer backend draws them through the CPU reference at a third of the window size and keeps the
frame until the command changes; the Vulkan backend will use the shader.

## Components

| Component | Call | Spec |
| --- | --- | --- |
| Button | `nk_ui_button(ui, id, label, flags)` | label centred, `LABEL` caps; 44 px (`_LARGE` 52 with `LABEL_L`, `_SMALL` 34 with `LABEL_S`); fits its label (`_GROW` fills); primary = accent fill, ink text; secondary = clear fill, `line_strong` edge, paper text; danger = `error` text; disabled = `dim` text, `line` edge (primary keeps a `surface_high` fill) |
| Game card | `nk_ui_tile(ui, id, &spec)` | `surface_raised`, `line` edge, radius 12; art panel flush with the top (tint per title, badge top-left; the disc's own `ICON0.PNG` once read, a showcase demo's picture, or the "ART FROM YOUR DISC" placeholder); name in `TILE_TITLE` (two lines); meta row: marker + status word in the status colour, disc ID right in `dim` |
| Add card | `nk_ui_add_tile` | ground fill, `line_strong` edge; 44 px outlined plus in accent, centred name and one-line note |
| Hero | `nk_ui_hero` | the title's art panel at 480:272 with a badge caption and the size note; the disc's `PIC1.PNG` fills it, else its `ICON0.PNG` sits centred, else the placeholder |
| Status pill | `nk_ui_status_pill` | 26 px, `surface_high`, `line_strong`, radius 4, `LABEL_S` in the status colour, marker first |
| Tag | `nk_ui_tag` | as the pill, `muted` text (YOUR DISC, SHOWCASE DEMO) |
| Badge | `nk_ui_badge` | 20 px on `art_scrim`, radius 4, `MICRO` |
| Eyebrow | `nk_ui_eyebrow` | `EYEBROW`; names a section or a card |
| Card | `nk_ui_card_begin` | `surface_raised`, `line`, radius 12, 16/20 px padding |
| Top bar | `nk_ui_top_bar` | N1 mark (28 px) and wordmark; section tabs in `LABEL` with a 3 px accent rule under the active one; input chip on the right (dot in `ready` for a controller) |
| Prompt bar | `nk_ui_prompt_bar` | `surface_raised`; an optional hint in `CAPTION` `muted` on the left, used only for a screen-specific tip (never a standing disclaimer); glyph (24 px, current prompt style) + `LABEL` per action on the right, 18 px apart |
| Vector art | `nk_ui_shapes` | rects, circles and convex polygons in a view box, fitted and centred; used for the N1 mark, the showcase demos' pictures and icons |

Prompt glyphs come from `nk_ui_prompt_glyph`, and the whole bar shows one set chosen from the active device
(`nk_ui_input_prompt_style`), never a mix. The player draws Kenney's CC0 "Input Prompts" glyphs
(`third_party/kenney/`, embedded by `tools/gen_ui_assets.py`), tinted `paper`; any glyph that is missing or fails to
decode falls back to the project-drawn geometric form, which tests and SDL-free builds also use:

- **Keyboard and mouse** (no pad connected, or a key or click was the last input): ENTER, ESC and the other keys as
  keycaps, the arrow keys for MOVE, and the top bar's chip says KEYBOARD AND MOUSE.
- **Controller** (a pad connected or was the last used): its D-pad for MOVE and its own face buttons, picked from the
  label SDL reports on the south button, never from the pad's name: A south gives the Xbox-style set, B south (A
  east) the Switch-style set, a cross the PlayStation-style set, and anything else the neutral position dots.
  Confirm and back follow position (south confirms), so a pad with A east shows B for confirm. The chip names the
  set.
- A pad that connects switches the set at once; when it disconnects the set falls back to another connected pad or
  to the keyboard. Pointer motion alone never switches.

Disc art is read on a background thread from the user's own disc image, one disc at a time, and decoded with
stb_image (`third_party/stb/`) inside fixed limits (512 x 512 for `ICON0.PNG`, 1024 x 1024 for `PIC1.PNG`, 1 MiB
encoded). It lives in memory only: nothing is written to disk or kept in the repository, and a disc without art, or
art that fails the checks, keeps the placeholder.

Optional launcher sounds (Kenney's CC0 "Interface Sounds": a tick when focus moves, a chime on confirm, a click on
back) are off by default; **UI sounds** in Settings (`ui_sounds` in `settings.json`) turns them on. While they are off
no audio device is opened, and they never play while a game runs.

Only actions the screen handles appear in its prompt bar.

## Screens

### Library (C5, built)

Top bar; `LIBRARY` in `DISPLAY` with the count in `CAPTION` beside it ("7 titles · 2 showcase demos"); a pager
(small PREVIOUS and NEXT buttons around "PAGE 1 OF 2") on the right of that row when the grid spans pages; the card
grid, ending with the add card ("Add a disc image you own", "Choose an .iso file"); prompt bar with MOVE and OPEN
and no corner text. Empty library: one centred 640 px card with a disc icon, `TITLE` "Add your first game", a `BODY`
line, the primary START SETUP WIZARD button (the default focus), CHOOSE A DISC IMAGE, and a caption about dropping an
.iso onto the window.

### Title detail (C5, built)

Aside: the hero, then a PACKAGE card (eyebrow, status pill, the status detail in `BODY_S` `muted`, and any input
note in an inset `info` box). The detail follows the package state (ready, checking, building, check failed,
missing, starting, running), never the primary action: while its game runs, a ready package says "Running now.
Stop the game to rebuild or change settings." The input note about decrypted files shows only while the package
still needs them. Main column: eyebrow LIBRARY › TITLE, the name in `TITLE_XL`, the disc ID in `DATA`, the status
pill and tag, one action row on the column's left edge with every button 52 px (the primary action, PLAY when
the package is ready, STARTING... while the launch starts, STOP GAME while a game runs, then OPEN SAVE FOLDER and
BACK), and the WHAT TO EXPECT card: one or two plain sentences on what to expect on this PC in `BODY_S` `paper`,
then the known issues in `muted` when there are any. It has no category chip: the `docs/COMPATIBILITY.md`
category describes the project's test route and read as a claim about the user's game (#1297), so only developer
mode names it, in a `FOOTNOTE`. Prompt bar: MOVE, SELECT, BACK.

### About (built)

Reached from the top bar's ABOUT tab (L / R or Q / E switch tabs; Back returns to the Library). A centred
880 px column:

- the N1 mark at 72 px beside `NAKAGAWA RECOMP` in `DISPLAY_XL`, with the version line under it in `LABEL_L`
  accent, exactly `v0.0.1 (MM-DD-YYYY)`;
- a `BODY_L` `muted` line saying what it is (a PSP static recompiler: your own game, recompiled to run natively),
  followed by the privacy promise;
- a LICENCE card (GPL-3.0-or-later, and a THIRD-PARTY NOTICES button that opens the existing licences screen) beside
  a CREDITS card (the upstream lineage in three lines, pointing at `CREDITS.md` and `NOTICE.md`);
- the non-affiliation notice ending "No game data is included.", the project link in `DATA`, and the short source
  commit as a `FOOTNOTE`.

The version is written once, `NK_PLAYER_VERSION` in `src/player/ui/player_ui_nk.h`. The date is the build's
`SOURCE_DATE_EPOCH` when set, else the source commit's time (Makefile and CMake both), formatted in UTC as
MM-DD-YYYY; without either the line reads `(dev build)`. No tag or release is implied by it.

### Settings (C6)

- Top bar with SETTINGS active; `SETTINGS` in `DISPLAY`; a tab row (`LABEL`, 10/12 px padding, 3 px accent rule
  under the active tab, `line` underneath): DISPLAY, AUDIO, CONTROLS, PATHS AND STORAGE, ACCESSIBILITY, ABOUT.
  L and R (Q and E) switch tabs.
- Body: a column of rows and, from 1100 px wide, a 340 px help panel (`card`, a 20 px Geist SemiBold heading,
  `BODY_S` `muted` text) that explains the focused row and names its `settings.json` key.
- Row: at least 56 px, 8/16 px padding, radius 8; label in Geist Medium 15 with a 12 px `muted` description; the
  control on the right. A focused row fills with `surface_high` and the ring surrounds the row.
- Segmented control: 3 px padding in a ground-filled, `line_strong`, radius 9 track; options 30 px, 13 px `muted`;
  the selected one `surface_high` with paper text, and a 2 px inset accent edge while its row has focus.
  Left and right change it.
- Toggle: 46 x 26 track (off `line_strong`, on accent) with a 20 px knob (off `muted`, on ink) and ON / OFF in
  `LABEL_S` before it.
- Slider: 280 px track 6 px high (accent fill, `line_strong` rest), 18 px paper thumb, the value in mono 13 right.
- Paths rows: name, the path in `DATA` (middle-truncated), status line, small actions (OPEN, REMOVE). No key-file
  row.
- Controls tab: a 30 px mapping header in `EYEBROW` and 38 px mapping rows; a row waiting for a press fills with
  `capture` and the prompt bar shows only CANCEL.

### First run (C7)

Built now: the setup wizard's six steps (Welcome, Options, Game, Verify, Fonts, Ready) on the existing wizard state,
in the dialog card the build and error screens use. Options ("Online and developer options",
[`ONLINE_FEATURES.md`](ONLINE_FEATURES.md)) comes right after Welcome because it needs no game: two switch rows (a
220 px button reading "Online mode: On" or "Off", accent while on, with its sentence beside it), the note that this
version has no online features yet, then CONTINUE (the default focus, so Enter never flips a switch), SKIP and BACK. A strip of step chips sits at the top (current: accent fill; done:
`ready` edge and a check); each step has one primary action first, then Back and Cancel. Every control returns
`PLAYER_UI_ACTION_WIZARD` with an op that `main.c` runs through the handlers the existing wizard calls. The Welcome
step says the privacy promise once. The left rail below is the target layout for a later pass:

- Left rail 260 px (32/24 px padding, `line` edge on the right) listing the steps: Welcome, Disc image, System
  fonts, Build, Play. There is no key-file step. Each step is 44 px with a 26 px numbered circle (`line_strong`
  edge); current: accent fill and ink number, name in SemiBold; done: `ready` edge and number; failed: `error`.
  Below 900 px the rail hides and the step shows as an eyebrow.
- Content: 36/56 px padding; `DISPLAY_XL` headline; `BODY_L` `muted` lead, at most 720 px wide; choice cards
  (`card`, 18 px padding, tag chip, `HEADING`, `BODY_S`); check rows (38 px, `line` underneath: name 170 px,
  code in `DATA` 140 px, status word 120 px in its colour, detail `CAPTION`).
- Build: stage rows 50 px with a 26 px circle (running: accent fill), name 170 px, and a 6 px progress bar (accent
  on `line_strong`); a log box (`#080B0F`, `line`, radius 10, mono 12/1.6 `muted`).
- One primary action per step at the bottom left (CONTINUE, BUILD, PLAY), secondary actions after it.

### Error dialogs (C8)

- Over the scrim, centred, 16 px from the edges; at most 700 px wide, or 880 px with a details box (a path list or
  a boundary), whose line budget grows by one per 16 px of window height past 540 so its fix is never cut off;
  `surface_raised`,
  `line_strong` edge, radius 16, 26/28/22 px padding, 14 px gap.
- Order: a status eyebrow in its colour with marker ("▲ CONTROLLER DISCONNECTED · GAME PAUSED"), a `TITLE` that says
  what happened in plain words, a `BODY` `muted` paragraph that says what to do, an optional detail box (ground
  fill, `line`, radius 10, `DATA` lines for paths or codes), then the button row: the recommended action first as
  primary, the way out last ("NOT NOW", "CANCEL"); a destructive action is a danger button and is never the
  default focus.
- Focus is trapped in the dialog and starts on the first button; BACK closes it with the safe choice.
- Cases to cover: unreadable or unsupported disc image (a store package is not a disc image), a different release of
  a known game (add as Experimental), build failure (stage named, log path shown), controller disconnected, quit to
  library. A disc whose program is encrypted gets the "does not decrypt games" explanation, never a request for a
  key file.

## Accessibility

- Text contrast is at least 4.5:1 for everything except `dim`, which is reserved for disabled controls and
  footnotes; over the brightest wave point muted and error text stay at 4.6:1.
- Focus ring 15.3:1 on the ground and at least 9.2:1 over the waves; high-contrast focus for any background.
- Targets are at least 44 px high; text size (90 to 150%) scales the whole layout through the density.
- Reduce motion holds the waves and makes transitions cuts.
- Launcher sounds are off by default and never replace a visual cue.

## Building and checking a screen

1. Compose the screen from the components above; give every element a stable id (sub-parts use
   `nk_ui_child_id`).
2. `mingw32-make nk-ui-player-screens-selftest` compares the draw-list dumps in `fixtures/nk_ui/*.golden` and runs
   the layout checks (ring outside the focused card, centred labels, grid gaps). After an intended change, run
   `build/test_player_screens --write-goldens` and say in the pull request why each golden changed.
3. `mingw32-make nk-ui-player-screens-capture NK_UI_CAPTURE_DIR=<dir>` saves offscreen captures of each state at
   1280 x 800, 1920 x 1200 at 150% and 960 x 600, from synthetic fixture titles only. Compare them with the
   design boards before review.
