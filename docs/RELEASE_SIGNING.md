# Release signing

Status: CURRENT. The player's **DOWNLOAD** on Settings > Updates fetches a release only when that release's
checksum file carries a valid signature from the project's update-signing key, and the public half of that key
is built into the player. This page is the owner's procedure: making the key, keeping it, signing each release,
and replacing the key. The signing tool is [minisign](https://github.com/jedisct1/minisign); this page and the
player's reader (`nk_minisign_verify` in `src/core/nk_sig.c`) follow minisign 0.12. Signing happens on an
offline machine. No secret key, password or signing step is ever in the repository or in CI.

## What is signed and what the player checks

Each release gets one signature: `nakagawa-recomp-<version>-windows-x64.zip.sha256.minisig`, a signature of the
ZIP's checksum file `nakagawa-recomp-<version>-windows-x64.zip.sha256`, attached to the release beside it. The
ZIP itself is not signed directly; its SHA-256 is in the signed file.

When the user selects **DOWNLOAD**, the player (`src/player/update_check.c`):

1. stops before any request if the build has no update-signing key, or if the release lists no checksum file or
   no `.minisig`;
2. fetches the checksum file and the `.minisig` (each at most 4096 bytes) from the release's own download URLs;
3. checks that the signature's key ID is the built-in key's, that the signature is valid for the exact bytes of
   the checksum file (the default prehashed `ED` form, or the legacy `Ed` form of `minisign -l`), and that the
   global signature over the trusted comment is valid;
4. only then reads the checksum file, which must name this release's ZIP. The ZIP's name carries the version,
   so a validly signed checksum file of another release, served under a newer tag, names another file and is
   refused (`UPDATE_CHECKSUM_WRONG_FILE`). That is the rollback and swap check: it relies on the signed bytes,
   not on the trusted comment, so it holds whatever comment a release was signed with;
5. fetches the ZIP and keeps it only if its SHA-256 matches, and matches GitHub's own digest of the asset when
   GitHub lists one.

Any failure stops the download with a card in plain words and leaves the release page open to the user:
`UPDATE_NO_SIGNING_KEY` (this build has no key), `UPDATE_SIGNATURE_MISSING` (the release is not signed),
`UPDATE_SIGNATURE_UNREADABLE` (the `.minisig` is not a minisign signature), `UPDATE_SIGNATURE_UNKNOWN_KEY`
(signed with another key) and `UPDATE_SIGNATURE_INVALID` (the checksum file or the trusted comment was
changed). The player reads the trusted comment's signature but not its text.

The signature proves that the checksum file is the one the key's holder signed. It does not stop someone who
controls the release list on GitHub from hiding a newer release, and it does not cover the release notes or
the release page.

## The public key in the build

The public key is the file `assets/update_signing_key.pub`, exactly as `minisign -G` writes it: an
`untrusted comment:` line naming the key ID, then one base64 line. The Makefile and `CMakeLists.txt` read it at
build time and compile its key line into the player as `NK_UPDATE_SIGNING_KEY`. The file is the only input, so
two builds of one commit carry the same key. A file that is there but is not a minisign public key stops the
build with "is not a minisign public key".

Until the owner commits that file, the build carries no key. Such a player still checks for updates and shows
the newer version, but says "This build can't check update signatures" and offers only the release page; a
download it is asked for fails with `UPDATE_NO_SIGNING_KEY` before any request. There is no placeholder key:
the file holds the owner's real public key or does not exist.

`mingw32-make --no-print-directory native-core-tests` runs `test_update_check`, which, when the file is there,
checks that the key the build embedded reads back.

## Make the key

Do this once, on a machine that is offline and stays offline while the secret key is in use.

1. Download the newest minisign release from [its releases page](https://github.com/jedisct1/minisign/releases)
   on another machine and check the archive's signature as the minisign README describes, with the public key
   published there. Copy it to the offline machine.
2. Make the key pair:

   ```powershell
   minisign -G -p update_signing_key.pub -s nakagawa-update.key
   ```

   minisign asks for a password, which encrypts the secret key file. Use a long passphrase of several random
   words, and keep it in a password manager, apart from the key file.
3. Keep `nakagawa-update.key` on the offline machine and on two removable drives, for example
   `<drive>:\nakagawa-update.key`, stored in different places. Never put it in the repository, CI secrets, a
   cloud-synced folder, email or chat. Losing either the key file or its password means replacing the key (see
   [Replace the key](#replace-the-key)).
4. Bring only `update_signing_key.pub` to the development machine and commit it as
   `assets/update_signing_key.pub`, unchanged. It is a new public path, so it needs provenance admission like
   any other. Note its key ID (the 16 hexadecimal digits in its first line) in the next release notes.

## Sign a release

After the package step in [`PREVIEW_RELEASE.md`](PREVIEW_RELEASE.md#recreate-the-package) writes
`nakagawa-recomp-<version>-windows-x64.zip.sha256`:

1. Copy that `.sha256` file, and nothing else, to the offline machine.
2. Sign it:

   ```powershell
   minisign -Sm nakagawa-recomp-<version>-windows-x64.zip.sha256 -s <drive>:\nakagawa-update.key `
       -t "nakagawa-recomp <version> windows-x64 zip checksum"
   ```

   This writes `nakagawa-recomp-<version>-windows-x64.zip.sha256.minisig` beside it, in minisign's default
   prehashed form; do not add `-l`. Keep the trusted comment to one short line.
3. Copy the `.minisig` back and check it against the committed public key:

   ```powershell
   minisign -Vm nakagawa-recomp-<version>-windows-x64.zip.sha256 -p assets/update_signing_key.pub
   ```

   Expected: "Signature and comment signature verified" and the trusted comment.
4. Attach the `.sha256` and the `.sha256.minisig` to the release next to the ZIP. Do not change the `.sha256`
   after signing, not even its line ends: the signature covers its exact bytes.

## Replace the key

The public key is compiled into each player, so a player trusts exactly the key it was built with.

- **Planned change.** Make a new key pair as above and commit the new `.pub` in place of the old one. The first
  release built with the new key is signed with the **old** key, so installed players can download it; every
  release after it is signed with the new key. Players older than that release see "Signed With Another Key"
  for later releases and are sent to the release page, so keep the changeover release as the newest one for a
  while before publishing the next.
- **Lost key or password.** Make a new key pair, commit it and sign the next release with it. Installed players
  refuse that release ("Signed With Another Key") and offer its release page, so users download it by hand once.
  Say so in the release notes.
- **Leaked key.** Do the same as for a lost key, and also publish a security advisory naming the leaked key ID.
  Players built with the leaked key keep trusting it until their users update by hand, so the advisory tells
  users to download the new version from the release page.

## Tests

The tests never touch the internet and never use the update-signing key. `tests/native/test_nk_sig.c` checks
`nk_minisign_verify` against real minisign 0.12 output made with a TEST-ONLY key whose secret half was deleted.
`tests/native/test_update_check.c` and `tests/native/test_player_ui.py` sign with TEST-ONLY keys derived from a
fixed label at run time: valid `ED` and `Ed` signatures, a changed checksum file or trusted comment, another key
ID, another key, cut or garbled base64, a missing `.minisig`, a build with no key, and another release's signed
checksum file.
