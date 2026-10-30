# Per-user data written by earlier players

Synthetic per-user files, each written by the player's own writer at an earlier
version, so the tests can prove that every format still loads, migrates forward
and round-trips after an update (`tests/native/test_player_state.c`,
`tests/native/test_input_profile.c`, `tools/test_nk_core.py`). The formats and
their version histories are in
[`docs/RUNTIME_PACKAGING_ARCHITECTURE.md`](../../docs/RUNTIME_PACKAGING_ARCHITECTURE.md#7-per-user-data-formats-and-versions).

Every value is invented: the disc IDs are the synthetic test IDs
(`tools/nk_core/synthetic_disc_ids.py`), the paths name no real user or drive,
and no file holds game data. Each file is the unmodified output of the writer
named below, built from that commit's sources with a small driver that filled in
the synthetic values; none was edited by hand.

| File | Format and version | Writer, at commit |
| --- | --- | --- |
| `v0.0.1/library.json` | library.json, schema 1 | `nk_library_save`, v0.0.1 (`d1c7d80d`) |
| `v0.0.1/settings.json` | settings.json, schema 2 | `player_app_save_settings`, v0.0.1 (`d1c7d80d`) |
| `v0.0.1/input_profile.json` | controller and keyboard profile, schema 3 | `nk_input_profile_file_save`, v0.0.1 (`d1c7d80d`) |
| `settings_v1.json` | settings.json, schema 1 | `player_app_save_settings`, `365953f2` (2026-10-08) |
| `input_profile_v1.json` | controller profile, schema 1 | `nk_input_profile_save`, `f8843deb` (2026-09-27) |
| `input_profile_v2.json` | controller profile with a per-title map, schema 2 | `nk_input_profile_file_save`, `d7baabf6` (2026-10-26) |

A test copies a fixture under its own temporary folder before it loads or saves
it, so a fixture is never rewritten in place.
