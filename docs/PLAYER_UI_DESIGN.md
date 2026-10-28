# Player UI design system

The visual spec for the native player's launcher UI (the only launcher since v0.0.1; the old renderer survives only in the regression-test build for
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
- **No key-file UI anywhere.** The player decrypts the user's own disc with built-in tables (#295) and reads no key
  file. No screen, step, setting, dialog or path row asks for, finds or names a key file. A disc whose program
  is encrypted gets the plain note the host supplies ("Nakagawa decrypts this game's program from your own disc, on
  this PC"), and one it can't decrypt gets the named reason.

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
- Arrow keys move to the nearest control in that direction. A button with `NK_UI_BUTTON_ROW` keeps Left and Right in
  its row, so a row of actions under cards of different heights is walked end to end.
- When the screen changes and the old focus is gone, focus lands on the page's first enabled control that may be the
  default: the title page's PLAY or BUILD PACKAGE, the new card after ADD TO LIBRARY. Top-bar tabs
  (`NK_UI_BUTTON_NOT_DEFAULT`) and danger buttons never are; a screen whose only control is a danger button (CANCEL
  BUILD while a build runs) starts with nothing focused, and a move reaches it. PLAY or BUILD PACKAGE gets focus back
  when it comes back on after STARTING... or a package check, unless the user moved meanwhile.

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
device density, and the logical width decides the columns. The launcher's screens are laid out on at least its
1280 x 720 design size (`NkUiViewport.min_logical_w` / `min_logical_h`): a smaller window, down to the 960 x 540
minimum, lowers the density so the same layout is drawn smaller, instead of pushing buttons and the prompt bar off
the window. A larger window keeps its density and gains room. Clay clips at most ten containers per layout: shorten
long text with `nk_ui_text_clamp` instead of clipping it.

Wrapped text keeps its wrapped height when its page is overfull. Clay took a paragraph's minimum height before
wrapping, as one line, so compressing an overfull column squeezed the paragraph back and the next block painted over
its later lines; the vendored `clay.h` carries the wrapped height into the minimum (`third_party/clay/PROVENANCE.md`).
`test_every_screen_fits_the_window` in `tests/native/test_player_screens.c` lays every screen out in launcher windows
of 1280 x 720, 960 x 540, 1366 x 705 and 853 x 480 pixels and fails when a box or text run leaves the window or text
is painted over.

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

Top bar with LIBRARY, ABOUT and SETTINGS tabs (SETTINGS opens the Settings view, whose page shows that tab active;
Start or F1, the menu prompt, opens it too); `LIBRARY` in `DISPLAY` with the count of the user's games in `CAPTION` beside it ("7 titles"); a pager
(small PREVIOUS and NEXT buttons around "PAGE 1 OF 2") on the right of that row when the grid spans pages; the card
grid of the user's games, ending with the add card ("Add a disc image you own", "Choose an .iso file"); prompt bar with MOVE, OPEN and
SETTINGS and no corner text. Empty library: one centred 640 px card with a disc icon, `TITLE` "Add your first game", a `BODY`
line, the primary START SETUP WIZARD button (the default focus), CHOOSE A DISC IMAGE, and a caption about dropping an
.iso onto the window.

TEST THIS PC: the demos that come with Nakagawa (the showcase packages beside the player) never join the grid. They
sit in their own row under it, or under the empty-library card while the user has no games, so the setup wizard is
offered on a first start even when the demos are there. The row's first grid column holds the `EYEBROW` "TEST THIS
PC" and one `CAPTION` line ("Play a demo that comes with Nakagawa to check graphics, sound and controls."); each
demo is a compact 72 px card one grid column wide (its project picture on the left, the name in `TILE_TITLE` over
two lines at most, and the status in `LABEL_S`). The cards are focusable, never the default focus, and open the
demo's title page like a game card. The grid gives up rows to fit the row; where the empty-library card and the row
do not both fit (the 960 x 540 minimum), the card leaves out its icon.

### Title detail (C5, built)

Aside: the hero, then a PACKAGE card (eyebrow, status pill, the status detail in `BODY_S` `muted`, and any input
note in an inset `info` box). The detail follows the package state (ready, checking, building, check failed,
missing, starting, running), never the primary action: while its game runs, a ready package says "Running now.
Stop the game to rebuild or change settings." The input note about decrypted files shows only while the package
still needs them. Main column: eyebrow LIBRARY › TITLE, the name in `TITLE_XL`, the disc ID in `DATA`, the status
pill and tag, one action row on the column's left edge with every button 52 px (the primary action, PLAY when
the package is ready, STARTING... while the launch starts, STOP GAME while a game runs, then OPEN SAVE FOLDER and
BACK), and the WHAT TO EXPECT card. Its eyebrow carries a player tier chip (PERFECT and PLAYABLE `ready`, IN-GAME
and MENU `info`, TITLE SCREEN `warning`, WON'T BOOT `error`; no chip for a title no one has tried). Below it come
one or two plain sentences on what to expect on this PC in `BODY_S` `paper`, then the known issues in `muted`
when there are any. The tier comes from the title's own player record, or else from its `docs/COMPATIBILITY.md`
status word through the table in [Status words](COMPATIBILITY.md#status-words). The status word itself describes
the project's test route and read as a claim about the user's game (#1297), so only developer mode names it, in a
`FOOTNOTE`. Under the card, REMOVE FROM LIBRARY (a `DANGER` button) for a disc the user added. It is left off
the showcase demos, and also while that game starts or runs, a package builds or a disc is being copied. Prompt
bar: MOVE, SELECT, BACK.

### Remove from library (built)

A flow card, 640 px wide, opened over the title page. Cancel has the first focus, and Back closes it, as Cancel
does. The eyebrow is REMOVE FROM LIBRARY with a `warning` triangle, then "Remove *title*?" in `TITLE`. A box lists
what Nakagawa built for the game, each item with its size: the built package, the build cache (packages and
bring-up), and the disc files Nakagawa copied. A `warning` FREES line gives the total. A `ready` line says the disc
image on the user's PC is not touched. The game's saves are the copied disc's Memory Stick folder and its
per-user save folder. They get their own box, unticked every time the dialog opens; under it, "Your saves stay." in
`muted`, or in `error` once the box is ticked, "Your saves for this game will be deleted too." A game with nothing
built and no saves says so. REMOVE (`DANGER`) removes the title, deletes those folders (and the saves only when
ticked), and returns to the library. Focus lands on the tile that took the removed title's place, or on the add
card when it was the last, and the page turns to it. The folders are found from the disc ID under the player's own
roots, never from a path stored in the library. The disc image, the user's decrypted files and the shared
manifests are never on the list. Prompt bar: MOVE, SELECT, CANCEL.

### About (built)

Reached from the top bar's ABOUT tab (L / R or Q / E switch tabs; Back returns to the screen it was opened from). A centred
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

### Game found (built)

The card a disc gets when it is added from the Library ([#1302](https://github.com/Jstar269/nakagawa-recomp/issues/1302)),
in the dialog card the wizard and error screens use, 820 px wide. A status pill (GAME FOUND `ready`, NOT TESTED YET
`info`, CAN'T PLAY YET or NOT ON THE LIST YET `error`) with the disc ID and YOUR DISC tags; the game's name in
`TITLE` ("PSP game" when the disc has none); one `BODY` lead saying what Nakagawa can do with it; then the checks in an
inset box, one row each: the status word in its colour (132 px), what was checked (124 px), and the sentence in
`muted`, two lines of it from a 680 px tall window and one below that. Every row is a player sentence from
`player_preflight_plain_message`, never the developer message, which stays in the log. Only a check that keeps the disc
out of the library is `error`; anything still to do (not built yet, PSP fonts missing) is `warning`, so nothing red
sits beside ADD TO LIBRARY. A disc this version cannot use is not offered to the library: its reason replaces the
lead's second line and the card offers CHOOSE ANOTHER DISC and BACK. While the disc is read, the card shows its path,
an indeterminate bar and CANCEL. The whole card fits the 960 x 540 minimum window.

### Settings (C6)

Built now (#1302): one page under the top bar with SETTINGS active, two cards of named rows, each value on the 34 px
button that changes it: GAME (resolution, frame pacing, VSync, fullscreen, scaling) and LAUNCHER AND SOUND (game
volume with - and +, launcher fullscreen, reduce motion, menu sounds, controller settings, then the controller and the
saves folder as captions). Under them DONE (primary: saves and returns to the Library), ONLINE AND DEVELOPER,
THIRD-PARTY NOTICES and BUILD TOOLS AND CACHE (#1306: the share libraries switch, CLEAR SHARED LIBRARY CACHE and
REMOVE BUILD TOOLS); the online options, the notices, build tools and cache and the removal check are dialog cards.
Left and Right move between the two cards along a row (`NK_UI_BUTTON_ROW`), so CONTROLLER SETTINGS is reached from
SCALING. Every change is saved as it is made. Below a 680 px tall window the `SETTINGS` heading is left out (the tab names the
page) so every row fits 960 x 540. The tabbed layout below is the target for a later pass:

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

### Controller settings (built)

Settings > Controls opens one page under the top bar with SETTINGS active. Under the `CONTROLS` heading (left out
below 680 px tall), CONTROLLER and KEYBOARD (the one shown filled; Up from the first binding) choose whose bindings the
page shows, beside one status line: a conflict in `error`, a load or save message, the prompt while a button is
awaited, or which controller is connected. Two cards side by side:

- BUTTONS: the eyebrow names the mapping being edited (`BUTTONS · GLOBAL MAPPING`, or the game's own in `ready`),
  then the 14 PSP controls in two columns of name and binding. The binding is a small button with the host button's
  name; a conflicted control's name is `error`; the control waiting for a press shows `Press a button...` as the
  primary button. Choosing it again, or Esc, cancels the wait. Between the name and the button a 20 px prompt glyph
  (`nk_ui_prompt_glyph_in`, #1299) shows the binding as the device shown prints it, whatever device drives the page:
  the controller in the set of the pad last used or connected (the neutral position dots with none), its face
  buttons by position, the shoulders, Start and the D-pad as the prompt bar draws them, and any other input as a pill
  named the way that pad names it (VIEW, ZL, L3); the keyboard as a keycap with the key's printed legend (SHIFT, ENTER,
  X) or the arrow keys. A control with nothing bound has no glyph.
- STICK AND TRIGGERS: dead zone and trigger threshold with - and +, CALIBRATE STICK, and LIVE INPUT in `DATA`: the
  raw stick, the PSP value it maps to, the triggers and the buttons held.

With KEYBOARD (#1299) the rows show each control's keys by their names on a US keyboard (`KEYS · GLOBAL MAPPING`),
the waiting control shows `Press a key...`, and the next key pressed is its key; Esc or a controller press stops the
wait. A key another control already uses, or one that cannot be bound (the function keys), is refused and the status
line says why in `error`: the profile refuses a key bound to two controls, and the runtime keeps Esc, F1 and F3. The
stick card names the keys Nakagawa keeps and says the analog stick has no keys.

Under the cards SAVE PROFILE (primary), RESET TO DEFAULTS (the device shown only) and, with a game selected, USE THIS
GAME'S OWN MAPPING or USE THE GLOBAL MAPPING; BACK sits at the right end, under the stick card. The page opens on the
first binding.
Calibration replaces the cards with a 720 px dialog card per step: rest (a progress bar, CANCEL only), range
(FINISH) and result (ACCEPT), each with its live numbers. Esc or East cancels a step and returns to CALIBRATE STICK.
Every page fits 960 x 540.

### Developer log (built)

F12 in developer mode opens one card under a top bar without tabs: a `warning` `▲ DEVELOPER MODE` eyebrow, the
privacy line (on this PC only, paths as `%USERPROFILE%`), then the newest lines that fit in `DATA` on the ground
colour, each cut to the card's width, and COPY, SAVE and CLOSE (primary, focused first) with the last notice beside
them. Esc or F12 closes it.

### First run (C7)

Built now: the setup wizard's six steps (Welcome, Options, Game, Verify, Fonts, Ready) on the existing wizard state,
in the dialog card the build and error screens use. Options ("Online and developer options",
[`ONLINE_FEATURES.md`](ONLINE_FEATURES.md)) comes right after Welcome because it needs no game: two switch rows (a
220 px button reading "Online mode: On" or "Off", accent while on, with its sentence beside it), the note that news
and reports aren't built yet, then CONTINUE (the default focus, so Enter never flips a switch), SKIP and BACK. A strip of step chips sits at the top (current: accent fill; done:
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
  library. A disc whose program can't be decrypted gets the plain reason and its folder, never a request for a
  key file.

### In-game overlay (C9c, built)

Drawn by the runtime over the frozen game frame under the `scrim` while the game is paused
(`src/rt/overlay/`, hosted by the pause presenter in `src/rt/gui.c`). No top bar and no waves: one card centred
16 px from the edges, and the prompt bar along the bottom. The card is the error dialog's (`nk_ui_dialog_begin`).

- Menu (480 px card): `GAME PAUSED` eyebrow in `muted`, the game's name in `TITLE` (two lines at most, else
  "Paused"), the disc ID in `DATA` `muted`, then a full-width column of buttons: RESUME (primary, 52 px, the
  default focus), SETTINGS, CONTROLS, QUIT TO LIBRARY. Prompts: MOVE, SELECT, RESUME.
- Settings (640 px): Settings rows as in C6. Game scaling is a segmented control (SHARP, INTEGER, SMOOTH; left and
  right step it, Confirm cycles it), the performance HUD a toggle. A `FOOTNOTE` says the changes apply now and last
  until the game closes; the player's Settings keep the saved values. BACK.
- Controls (640 px, or 880 px in two side-by-side tables when the window is under 720 px high and there are more
  than six rows): a PSP / KEYBOARD / CONTROLLER header in `EYEBROW` and 38 px rows (34 px when split) with `line`
  dividers; an unbound control shows an em dash. The rows describe the mapping in force: the window's own key
  table and the input profile, with the pad's buttons named for its family (A, B, X, Y and LB for an Xbox-style
  pad, Cross and L1 for a PlayStation-style one, the position for an unknown pad), the prompt bar's rule. A
  `FOOTNOTE` points at the player's Settings for remapping. BACK.
- Quit to library (560 px): `▲ QUIT TO LIBRARY` in `warning`, "Quit" and the game's name, "Anything the game has not saved is
  lost.", then KEEP PLAYING (primary, the default) and QUIT TO LIBRARY (danger, never the default). While a save is
  in progress the dialog says so ("The game is saving") and offers RESUME and BACK only.
- Controller disconnected (560 px): `▲ CONTROLLER DISCONNECTED · GAME PAUSED` in `warning` and what to do; once a
  pad is back, `● CONTROLLER CONNECTED · GAME PAUSED` in `ready`. RESUME (primary) and PAUSE MENU.
- Input: Esc and the pad's back button go back a page and resume from the menu; F1 and the pad's Start resume from
  any page; Guide resumes from anywhere. Back on the controller dialog goes to the menu, never into the game.
- Without SDL3_ttf or the bundled fonts the runtime shows the frozen frame under the scrim alone (C9b's overlay),
  and says why once on stderr; Esc, F1, Guide and the pad's back button resume.

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
4. The in-game overlay's pages have their own gates: `mingw32-make nk-ui-overlay-screens-selftest` (flow, focus,
   fit at those three sizes, and `fixtures/nk_ui/overlay.golden`; rewrite it with
   `build/test_overlay_screens --write-goldens`) and `mingw32-make nk-ui-overlay-host-selftest` (the runtime host on
   SDL3_ttf, the bundled fonts and real key events).
