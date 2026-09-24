# Playing your own games

Status: CURRENT. This guide explains what the native player needs in order to
play a PSP game you own, what it does for you automatically, and what is still
in the works.

## What you need

- **A disc image of a game you own.** Use an ISO made from your own UMD, or your
  own PlayStation Store purchase. Nakagawa Recomp never downloads games and
  contains no game data. Don't use copies you don't own.
- **The game's executable in unencrypted form, for most commercial games.**
  Nakagawa recompiles the game's own program code into a native program on your
  computer, so it has to be able to read that code. Most retail PSP executables
  (`EBOOT.BIN` and some `.prx` modules) are encrypted.

## What the player does automatically

1. Add the ISO: drag it into the player window, or click **Add Game**.
2. The player reads the disc's own `PARAM.SFO` to identify the title and disc ID.
3. It checks the executable:
   - An **unencrypted** executable, including discs that carry an unencrypted
     `BOOT.BIN`, is selected automatically. Homebrew and the project's showcase
     demos are in this group.
   - For an **encrypted** executable, the game card says so and names the
     folder where the player looks for your unencrypted files.
4. Once an unencrypted executable is available, click **Build package**. The
   player recompiles the game, shows each stage, and turns on **Play** when the
   package is ready. Anything it can't handle yet stops the build with a message
   naming what is missing and its tracking issue. It never pretends a build
   succeeded.

## Supplying unencrypted files

There are community tools that produce an unencrypted copy of a game's
executable and modules from your own copy. Some run on your own PSP, others on
a PC. Nakagawa Recomp doesn't include, endorse or link to any of them. It
contains no decryption keys and needs none.

Put the unencrypted files in the per-title folder the player shows on the game
card:

```text
<user data>/titles/<DISC_ID>/decrypted/
├── EBOOT.elf        (the game's main executable)
└── <module>.prx     (only modules the player or build says are required)
```

On Windows, `<user data>` is `%LOCALAPPDATA%\Nakagawa\data`. A usable file
starts with the ELF signature bytes `7F 45 4C 46`. A file that starts with
`~PSP` or `~SCE` is still encrypted, and the player will say so. The player
picks up the folder automatically the next time it checks the game.

Laws on decrypting software differ between countries, and some restrict it
even for copies you own. Check the rules where you live.

## Keep it to your own games

- Only use games you own.
- Don't share decrypted files, generated packages or disc images. Everything
  Nakagawa builds stays in your user data folder.
- Issues, pull requests and project discussions must never contain game files,
  keys, or links to downloads or decryption tools.

## What is still in the works

| Area | Today | Tracking |
| --- | --- | --- |
| Encrypted executables | You supply unencrypted files as described above. The project is reviewing lawful ways to make this automatic. | [#295](https://github.com/Jstar269/nakagawa-recomp/issues/295) |
| Commercial-game packages in public builds | Public builds can package homebrew, showcase demos and synthetic fixtures. Commercial titles still need runtime pieces that are being rewritten for the public tree. | [#297](https://github.com/Jstar269/nakagawa-recomp/issues/297), [#349](https://github.com/Jstar269/nakagawa-recomp/issues/349) |
| In-game system fonts | Import fonts from your own PSP with `python tools/nk_cli.py fonts import <folder>`. | [#300](https://github.com/Jstar269/nakagawa-recomp/issues/300) |
| Games beyond the verified title | Other games import as Experimental; bring-up of further titles is ongoing. | [#285](https://github.com/Jstar269/nakagawa-recomp/issues/285), [#308](https://github.com/Jstar269/nakagawa-recomp/issues/308) |

See [`SETUP.md`](SETUP.md) for developer setup and the command-line tools.
