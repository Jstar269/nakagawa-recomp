# PROVENANCE.md — `third_party/dr_libs/`

The single-file WAV reader and writer from dr_libs, vendored for selftests
only. `tests/native/pcm_wav_dump.c` uses its writer to save decoded PCM (the
ATRAC3+ bridge selftest's canaries today) as WAV files when `NK_PCM_WAV_DIR`
is set, and `tests/native/test_pcm_wav_dump.c` reads those files back. It is
not compiled into the native player, the runtime, or any shipped artifact.

## Upstream source

- Project: dr_libs (dr_wav)
- Repository: `https://github.com/mackron/dr_libs`
- Pin: commit `d422edf14ec2b81ac40a7f22b5a7f64276b5fc34` (2026-10-23), the head
  of the default branch `master` at the time of import. The header names its
  version `v0.14.6 - TBD`: upstream has no tags, and this commit carries the
  bug fixes made to `dr_wav.h` since the v0.14.5 commit `fa931f3285ce` (the
  newest, `14423ee630e4`, fixes an initialization error).
- License: the choice of `Unlicense` (public domain) or `MIT-0`. The project
  elects the Unlicense (public domain) option, under the owner's 2026-10-24 rule
  that officially public-domain code may be vendored with the normal
  bookkeeping. Both alternatives are kept, unchanged, at the end of `dr_wav.h`
  and in `LICENSE.txt`.
- Import method: both files were downloaded from `raw.githubusercontent.com`
  at the pinned commit. Upstream `LICENSE` is stored as `LICENSE.txt`, bytes
  unchanged, so the publication classifier recognises it as a licence text.
  Their Git blob IDs match the files in that commit, and the SHA-256 values
  below were calculated from the local bytes.

## File manifest

| File | Kind | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `dr_wav.h` | imported byte-identical | `6d4da204965c1fc1710ded1d324834340b3ce850` | `ba2b6a7dd278bb11321e3bfe32975cbe080c5b38f249dcc4a239b8650b5e0f97` |
| `LICENSE.txt` | imported byte-identical (upstream `LICENSE`) | `16dfbdfda2b761d1ec2ed57266dbdc777349ad25` | `dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Local deltas

None. Two whitespace-only lines in upstream `dr_wav.h` end in spaces; the
repository's `.gitattributes` exempts that one file from the trailing-space
check so the bytes stay identical to upstream.
