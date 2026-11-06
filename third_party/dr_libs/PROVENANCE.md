# PROVENANCE.md — `third_party/dr_libs/`

The single-file WAV reader and writer from dr_libs, vendored for selftests
only. `tests/native/pcm_wav_dump.c` uses its writer to save decoded PCM (the
ATRAC3+ bridge selftest's canaries today) as WAV files when `NK_PCM_WAV_DIR`
is set, and `tests/native/test_pcm_wav_dump.c` reads those files back. It is
not compiled into the native player, the runtime, or any shipped artifact.

## Upstream source

- Project: dr_libs (dr_wav)
- Repository: `https://github.com/mackron/dr_libs`
- Pin: tag `wav-0.14.6`, commit `35b5c392cc15456736c440a68dbe073178bbffe0`
  (2026-10-30, "dr_wav: Version 0.14.6"), also the head of the default branch
  `master` when re-pinned on 2026-11-05. The header names its version
  `v0.14.6 - 2026-10-31`. The previous pin was the untagged pre-release commit
  `d422edf14ec2b81ac40a7f22b5a7f64276b5fc34` (2026-10-23, header
  `v0.14.6 - TBD`); the release adds fixes to fact-chunk sample counts, AIFF
  ADPCM/a-law/mu-law decoding, u8-to-f32 conversion, reads into a NULL buffer
  and metadata parsing. None of those paths is used here: the selftests write
  and read back RIFF PCM through `drwav_write_pcm_frames` and
  `drwav_read_pcm_frames_s16` into real buffers, without metadata.
- License: the choice of `Unlicense` (public domain) or `MIT-0`. The project
  elects the Unlicense (public domain) option, under the owner's 2026-10-24 rule
  that officially public-domain code may be vendored with the normal
  bookkeeping. Both alternatives are kept, unchanged, at the end of `dr_wav.h`
  and in `LICENSE.txt`.
- Import method: both files were downloaded from `raw.githubusercontent.com`
  at the pinned commit (`LICENSE` is unchanged since the previous pin). Upstream `LICENSE` is stored as `LICENSE.txt`, bytes
  unchanged, so the publication classifier recognises it as a licence text.
  Their Git blob IDs match the files in that commit, and the SHA-256 values
  below were calculated from the local bytes.

## File manifest

| File | Kind | Upstream blob SHA | SHA-256 |
| --- | --- | --- | --- |
| `dr_wav.h` | imported byte-identical | `f7920db42ff78e66cb74489435f8772ccc4d4977` | `fad60f1e360d6de726efc1b9a7985610c89c551719e3f110e8b992f0e1f308cd` |
| `LICENSE.txt` | imported byte-identical (upstream `LICENSE`) | `16dfbdfda2b761d1ec2ed57266dbdc777349ad25` | `dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15` |
| `PROVENANCE.md` | project-authored manifest | n/a | n/a |

## Local deltas

None. Two whitespace-only lines in upstream `dr_wav.h` end in spaces; the
repository's `.gitattributes` exempts that one file from the trailing-space
check so the bytes stay identical to upstream.
