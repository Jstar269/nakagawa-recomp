# Data-Driven Title Profile Architecture

## 1. Principle: Decoupling Titles from Core UI

Nakagawa Recomp was initially developed focusing on *Hot Shots Tennis: Get a Grip* (UCUS98701). To achieve long-term success and scale across multiple PSP titles:
**Zero title-specific addresses, filenames, disc IDs, memory offsets, or compatibility hacks may be hardcoded into generic UI or engine code.**

Titles are modeled as **declarative title profiles** managed by a portable **Title Registry**.

```text
┌────────────────────────────────────────────────────────┐
│                   GENERIC NAKAGAWA CORE                │
│    (Title Registry · ISO Inspector · Prep Engine)      │
└───────────▲────────────────────────────────▲───────────┘
            │                                │
┌───────────┴──────────┐         ┌───────────┴──────────┐
│ HST Title Profile    │         │ Future Title Profile │
│  - UCUS98701 (US)    │         │  - ULUS10xxx         │
│  (Private local)     │         │  - Genuine modules   │
└──────────────────────┘         └──────────────────────┘
```

---

## 2. The Multi-Title Design Principle

> [!IMPORTANT]
> **TITLE PROFILES DESCRIBE TITLES, NOT COMPATIBILITY PATCHES.**
>
> A title profile is a structural manifest describing what a game disc contains. It is **never** a vehicle for injecting hacks, register patches, fake return values, or module replacements into the runtime.
>
> **Regional Independence Note:** Regional releases (e.g. EU vs US) are distinct binaries. Different regional releases may have different code layouts, relocation offsets, and function entry points. They cannot be assumed binary-compatible without individual verification.

### 2.1 Good Profile Data (Declarative Structural Metadata)
- **Disc Identity:** Disc ID (e.g. `TEST00001`, `UCUS98701`), region, disc version, disc title.
- **Executable Inventory:** Executable paths on disc (`SYSDIR/EBOOT.BIN`, `BOOT.BIN`).
- **Module Inventory:** Required PRX module filenames and dependencies (`libfont.prx`, `scePsmf_library.prx`).
- **Container Metadata:** Archive format (`claphanz_xb`, `raw`) and container location on disc.
- **Filesystem & Save Namespaces:** Expected save directory names and unique partition identifiers.
- **Preparation & Runtime Version:** Versioned toolchain markers and verification digests.

### 2.2 Prohibited Profile Fields (Compatibility Hacks)
Title profiles must **never** contain fields equivalent to:
- `patch_x` / `hook_address_y`
- `fake_return_z`
- `replace_module_with_hle`
- `clobber_guard_register`
- `force_walker_exit`

When a new PSP game fails to boot or encounters an issue on Nakagawa, the problem must be resolved by fixing the generic CPU recompiler, the LLE loader, or the generic kernel implementation. This ensures that every engine improvement benefits all current and future titles.

---

## 3. Title Profile Schema Specification

```json
{
  "$schema": "./title_profile.schema.json",
  "id": "hst-ucus98701",
  "name": "Hot Shots Tennis: Get a Grip! (North America)",
  "disc_ids": ["UCUS98701", "UCUS-98701"],
  "regions": ["NA"],
  "executable": {
    "path": "PSP_GAME/SYSDIR/EBOOT.BIN",
    "base": 0,
    "entry": 0
  },
  "required_modules": [
    "PSP_GAME/USRDIR/module/libfont.prx",
    "PSP_GAME/USRDIR/module/scePsmf_library.prx",
    "PSP_GAME/USRDIR/module/scePsmfP_library.prx"
  ],
  "archive": {
    "format": "claphanz_xb",
    "path": "PSP_GAME/USRDIR/data.xb",
    "mount_vfs": true
  },
  "runtime": {
    "profile": "hst",
    "save_namespace": "UCUS98701"
  }
}
```

---

## 4. Title Agnosticism in the User Interface

The desktop UI and player frontend are 100% title-agnostic:
1. The UI interacts solely with the `TitleRegistry` and `IsoInspector`.
2. When the user drops an ISO, the ISO inspector reads `PARAM.SFO` directly from the raw disc bytes and queries `TitleRegistry.find_by_disc_id(disc_id)`.
3. If matched, the UI displays the title's official metadata and launches the generic preparation engine.
4. Supporting a new title in the UI requires zero code changes to presentation components—only registering a new valid profile.

---

## 5. Local Title Manifest (`manifest.json`)

When an ISO is prepared for the first time, the preparation engine writes a private, local manifest into the game's local application data folder (`%LOCALAPPDATA%/nakagawa/titles/<disc_id>/manifest.json`):

```json
{
  "schema_version": 1,
  "engine_version": "0.2.0",
  "title_id": "hst-ucus98701",
  "disc_id": "UCUS98701",
  "title_name": "Hot Shots Tennis: Get a Grip! (North America)",
  "iso_path": "C:\\Games\\PSP\\HotShotsTennis.iso",
  "iso_size": 1288765440,
  "created_at": 1757123456.78,
  "runtime_profile": "hst",
  "archive_format": "claphanz_xb",
  "save_namespace": "UCUS98701",
  "status": "READY"
}
```

This manifest provides an explicit, deterministic session contract for the runtime launcher, completely removing reliance on ambient environment variables (`PSP_ISO`, `SR_DATAROOT`).
