# Failure codes

Every failure Nakagawa Recomp reports carries a stable code. This page lists all of them, grouped
by the pipeline stage that produces them, and says what each one means.
[`assets/failure_codes.json`](../assets/failure_codes.json) is the machine-readable form of the
same list. `tools/test_failure_codes.py` checks that this page, that file and every program that
emits a code agree, so a code that appears in a report, a log or the player is always listed
here.

## How codes work

- **Stable.** A code is never renamed, and its meaning never changes. When nothing emits a code
  any more it moves to the `retired` list in `failure_codes.json` and is not reused.
- **Named, not bucketed.** There is no "unclassified" code. When a stage stops for a reason no
  more specific code covers, the code says what is known: `ANALYZER_INTERNAL_ERROR`, for
  example, means the analyzer itself failed (a defect in Nakagawa), not that the game is
  unsupported.
- **One stage each.** Each code belongs to one stage, the place in the pipeline where it
  normally stops a title. Some codes can also stop an earlier or later stage: the import-table
  codes, for example, can stop the import stage when a guest module's table is planned there.

The stages, in pipeline order:

| Stage | What runs | Bring-up report stage |
| --- | --- | --- |
| `inspect` | Reading the disc image, its title information and its executable layout | `inspect` |
| `decrypt` | Built-in decryption of the game's own executable and modules | runs before `inspect` |
| `import` | Staging the game's files and guest modules and planning their load addresses | `prepare_import` |
| `analyze` | Finding the code, imports and executable regions in the executable and modules | `analyze` |
| `codegen` | Generating C from the analyzed code | `codegen` |
| `compile` | Compiling the generated C with the runtime | `compile` |
| `package` | Assembling the runnable package | `build_package` |
| `run` | Launching the package and watching the game start | `launch` |
| `host` | The player and the tools themselves: downloads, updates, settings, the library | none |

## Where codes appear

The **Where** column of each table names the places a code can appear:

| Name | Where |
| --- | --- |
| `report` | The bring-up report's `failure_class` (`nk_cli.py bringup`, [`assets/bringup_report.schema.json`](../assets/bringup_report.schema.json)). `tools/library_build.py`, `tools/library_sweep.py` and `tools/title_qualification.py` record the same value. |
| `route` | The code a tool stops with and prints, such as `title_codegen_plan.py` (`PackageRouteError`), the import-table reader (`ImportTableError`) and the disc inspector (`IsoInspectionError`). A bring-up report names these through the `report` code they map to. |
| `staging` | The code a game-file staging run returns (`nk_core.prep_engine`). |
| `decrypt` | The built-in decryption result: `nk_psp_status_name`, the status word `nk_decrypt` prints, the player's disc check, and `decrypt_step.first_failure_code` in a `library_build` record. |
| `disc` | A disc image or PlayStation Store package the player or the tools refuse (`nk_disc_expand`, the PBP rules in `nk_iso.c`). |
| `player` | The `Code:` line of the player's problem report. |
| `library` | A `tools/library_build.py` or `tools/library_sweep.py` record, beyond the report codes those tools copy. |

## inspect

| Code | Where | Meaning |
| --- | --- | --- |
| `DISC_IDENTIFY_FAILED` | library | The disc's title information could not be read, so the library could not identify the disc. The record's `identify_detail` says why. |
| `DISC_IMAGE_BLOCK_CORRUPT` | disc, player | A compressed disc image has a block that does not decompress. |
| `DISC_IMAGE_CHD_CD_UNSUPPORTED` | disc, player | The CHD file holds a CD image, not a PSP UMD. |
| `DISC_IMAGE_CHD_INVALID` | disc, player | The CHD file is damaged or not a CHD file. |
| `DISC_IMAGE_CHD_UNAVAILABLE` | disc, player | This build cannot read CHD files. |
| `DISC_IMAGE_CHD_UNSUPPORTED` | disc, player | The CHD file uses a layout or codec Nakagawa does not read. |
| `DISC_IMAGE_EXPANDED_INVALID` | disc, player | The compressed file expanded, but the result is not a readable disc image. |
| `DISC_IMAGE_HEADER_INVALID` | disc, player | The compressed disc image's header is invalid. |
| `DISC_IMAGE_ID_MISSING` | disc, player | The disc image has no disc ID in its title information. |
| `DISC_IMAGE_INDEX_INVALID` | disc, player | The compressed disc image's block index is invalid. |
| `DISC_IMAGE_NOT_COMPRESSED` | disc, player | The file given to the disc expander does not start with a CSO, ZSO or CHD signature. |
| `DISC_IMAGE_NOT_EXPANDED` | disc, player | A compressed disc image was given to the disc inspector, which reads only the plain ISO made when the image is added to the library. |
| `DISC_IMAGE_READ_FAILED` | disc, player | The disc image could not be read. |
| `DISC_IMAGE_REFUSED` | report | A compressed disc image could not be expanded into a plain ISO; the run's output names the `disc` code. |
| `DISC_IMAGE_SIZE_INVALID` | disc, player | The compressed image's header names an expanded size no PSP disc has. |
| `DISC_IMAGE_TRUNCATED` | disc, player | The disc image ends before its data does. |
| `DISC_IMAGE_VERSION_UNSUPPORTED` | disc, player | The compressed disc image uses a format version Nakagawa does not read. |
| `DISC_IMAGE_WRITE_FAILED` | disc, player | The expanded disc image could not be written. |
| `DISC_NOT_PSP` | disc, player | The disc image is not a PSP game: it has no `PSP_GAME` folder. |
| `EXECUTABLE_UNSUPPORTED` | report | The disc's executable is missing, encrypted with no decrypted copy, or breaks a PSP layout rule. |
| `INVALID_ISO` | report | The input is not a readable PSP disc image, or its title information is invalid. |
| `ISO_CORRUPT` | player | The picked file is not a PSP disc image. |
| `MODIFIED_DUMP_CFW_LOADER` | report | The disc image was modified by a custom-firmware patch; its real executable needs a decrypted copy. |
| `PBP_HEADER_TRUNCATED` | report, route, disc, player | The PlayStation Store package (PBP) ends inside its header. |
| `PBP_OFFSETS_INVALID` | report, route, disc, player | The PBP's section offsets are out of order or outside the file. |
| `PBP_PACKAGE_UNSUPPORTED` | report, route, disc, player | The PBP is not a PSN release of a UMD game, the only kind Nakagawa converts. |
| `PBP_SFO_INVALID` | report, route, disc, player | The PBP's title information (`PARAM.SFO`) cannot be read. |
| `SOURCE_NOT_FOUND` | player | The game's source file is no longer where the library recorded it. |

## decrypt

| Code | Where | Meaning |
| --- | --- | --- |
| `CONTEXT_REQUIRED` | decrypt, player | The file is bound to the console that installed it (its fuse ID), which Nakagawa cannot take, so it is refused. |
| `DECOMPRESS_FAILURE` | decrypt, player | The decrypted payload does not decompress; the file looks damaged or changed. |
| `FIRMWARE_OUT_OF_SCOPE` | decrypt, player | The file is PSP system software. Nakagawa never decrypts firmware. |
| `INTEGRITY_FAILURE` | decrypt, player | The file's signature or hash check failed; the file looks damaged or changed. |
| `IO_FAILURE` | decrypt, player | The input could not be read or the output could not be written. |
| `KEY_FILE_INVALID` | decrypt, player | The supplied key file is missing or invalid. |
| `KEY_REQUIRED` | decrypt, player | The file needs a key the built-in table does not carry; `nk_decrypt` names the entry. |
| `MALFORMED_INPUT` | decrypt, player | The encrypted container is malformed. |
| `OUTPUT_EXISTS` | decrypt, player | A file is already where the decrypted copy would go. |
| `PSONE_CLASSIC_UNSUPPORTED` | decrypt, disc, player | The package is a PSOne Classic, a PlayStation game for the PSP's emulator, which is out of scope. |
| `RESOURCE_LIMIT` | decrypt, player | The file on the disc is empty or over the decryption size limit, or memory for it could not be allocated. |
| `UNSUPPORTED_SCHEME` | decrypt, player | The file uses an encryption form Nakagawa does not cover yet. |
| `VALIDATION_FAILURE` | decrypt, player | The decrypted file failed its check, so it was not kept. |

## import

| Code | Where | Meaning |
| --- | --- | --- |
| `DISC_FILES_STAGE_FAILED` | report | The game's files on the disc could not be staged; the output says why. |
| `EXECUTABLE_LOAD_BINDING_REQUIRED` | route | A relocatable executable has no load address bound to it. |
| `EXPERIMENTAL_CODEGEN_PROFILE_MISSING` | route | An experimental title's profile has no `codegen_profile` setting and is not the player's untouched default profile. |
| `EXPERIMENTAL_IMPORT_FAILED` | report | An experimental title's files could not be imported; the output names the step. |
| `EXPERIMENTAL_PROFILE_FAILED` | player | An experimental title's profile could not be saved. |
| `FIXED_ADDRESS_EXECUTABLE_OUTSIDE_GUEST_RAM` | route | A fixed-address executable loads outside PSP user memory. |
| `GAME_FILES_NOT_READY` | player | The game's files have not been set up yet. |
| `GENERIC_EXPERIMENTAL_PROFILE_UNAVAILABLE` | route | The executable's layout does not fit the generic experimental profile, so the profile could not be rebuilt. |
| `GUEST_MODULE_DECRYPTION_REQUIRED` | report, route | A guest module is encrypted and no decrypted copy is in the title's folder. |
| `GUEST_MODULE_DISCOVERY_FAILED` | report, route | The disc's guest modules could not be listed, for example two modules share a name. |
| `GUEST_MODULE_FORMAT_UNSUPPORTED` | report, route | A guest module breaks a PSP layout rule or uses kernel-mode code. |
| `GUEST_MODULE_LOAD_ADDRESS_LAYOUT_UNAVAILABLE` | report, route | The guest modules and the executable do not fit in PSP user memory together. |
| `GUEST_MODULE_LOAD_BINDING_REQUIRED` | report, route | A guest module needs an explicit load address. |
| `GUEST_MODULE_STAGE_FAILED` | report, route | A guest module could not be copied into the title's folder. |
| `ISO_UNQUALIFIED_REVISION` | staging | The disc's title needs a local compatibility record, and none is registered or the recorded one no longer matches the disc. |
| `ISO_UNSUPPORTED_TITLE` | staging | No catalogued title profile matches the disc's ID. |
| `PLAYER_NOT_FOUND` | staging | The native player, which stages the files, was not found. |
| `PLAYER_START_FAILED` | staging | The native player could not be started to stage the files. |
| `PREPARATION_CANCELLED` | staging | Staging was cancelled. |
| `PREPARATION_FAILED` | staging | Staging failed; the message says why. |
| `PREPARATION_OUTPUT_INVALID` | staging | Staging finished but its output failed the check. |
| `RELOCATABLE_ELF_LOAD_BINDING_REQUIRED` | report | The executable is relocatable and needs a load address binding. |
| `RUNTIME_HLE_REGISTRY_UNAVAILABLE` | report, route | The runtime's list of built-in system functions could not be read; the installation may be incomplete. |
| `TITLE_PROFILE_MISSING` | player | No catalogued title manifest matches the library entry's title ID. |

## analyze

| Code | Where | Meaning |
| --- | --- | --- |
| `ANALYZER_IMPORT_LIBRARY_NAME_UNMAPPED` | report, route | An import entry's library-name pointer is null, or the name runs outside mapped data. |
| `ANALYZER_IMPORT_NID_TABLE_MISSING` | report, route | The import table declares functions without NID data. |
| `ANALYZER_IMPORT_REGIONS_MISMATCH` | report, route | The import stub and NID regions do not pair one to one. |
| `ANALYZER_IMPORT_TABLE_INVALID` | report, route | The import table is invalid or outside mapped data. |
| `ANALYZER_INTERNAL_ERROR` | report | The analyzer stopped on an internal error. This is a defect in Nakagawa, not an unsupported game feature; the run's private diagnostic names it. |
| `ANALYZER_VARIABLE_IMPORTS_UNSUPPORTED` | report, route | The executable imports variables, which are not supported yet. |
| `PACKAGE_INVALID_ELF` | route | The executable or a module is not a valid ELF image. |
| `PACKAGE_INVALID_IMPORT_TABLE` | route | The import table could not be read. |
| `PACKAGE_MODULE_DIR_REQUIRED` | route | A guest module was planned without the folder that holds it. |
| `PACKAGE_NO_EXECUTABLE_REGIONS` | route | The image has no file-backed executable region. |

## codegen

| Code | Where | Meaning |
| --- | --- | --- |
| `CODEGEN_FAILED` | report | C generation failed. |
| `PACKAGE_INVALID_CODEGEN_REPORT` | route | The code generator's report is missing or invalid. |

## compile

| Code | Where | Meaning |
| --- | --- | --- |
| `COMPILE_FAILED` | report | The generated C did not compile. |
| `PACKAGE_TOOLCHAIN_MISSING` | route | The C compiler or a build tool was not found. |
| `PACKAGE_TOOLCHAIN_VERSION_FAILED` | route | Make or the C compiler did not report a version. |
| `PRODUCTION_RUNTIME_BACKEND_UNAVAILABLE` | report | A private-backend build (`PUBLIC_SAFE=0`) needs the PGF and PGD backends, which this checkout does not have. |

## package

| Code | Where | Meaning |
| --- | --- | --- |
| `BUILD_PACKAGE_FAILED` | report | The package could not be assembled. |
| `PACKAGE_AMBIENT_ENHANCEMENT` | route | An enhancement setting from the environment would change the package; it must be explicit. |
| `PACKAGE_BUILD_FAILED` | route, player | The package build failed; the problem report's reason gives the tool's message. |
| `PACKAGE_BUILD_INCOMPLETE` | route | The build finished without every file the package needs. |
| `PACKAGE_BUILD_IN_PROGRESS` | player | A package build is already running. |
| `PACKAGE_BUILD_REFUSED` | player | The build's preflight refused to start it. |
| `PACKAGE_DISK_FULL` | player | The disk filled up during the build. |
| `PACKAGE_INPUT_CHANGED` | route | An input changed while the package was being built. |
| `PACKAGE_INPUT_MISSING` | route | An input file the package needs is missing. |
| `PACKAGE_INVALID_ENHANCEMENT` | route | An enhancement setting is invalid. |
| `PACKAGE_INVALID_GAME_NAME` | route | The game name is not a valid package name. |
| `PACKAGE_INVALID_IDENTITY` | route | The title input identity is unreadable, or does not match the manifest, executable or modules being built. |
| `PACKAGE_INVALID_MANIFEST` | route | The title manifest is invalid. |
| `PACKAGE_INVALID_PATH` | route | A path given to the build is invalid. |
| `PACKAGE_INVALID_PLAN` | route | The build plan is invalid. |
| `PACKAGE_LICENSE_RECORD_MISSING` | route | A bundled component has no licence record. |
| `PACKAGE_LICENSE_TEXT_MISSING` | route | A bundled component's licence text is missing. |
| `PACKAGE_OUTPUT_CONFLICT` | route | The output folder holds files from a different build. |
| `PACKAGE_OUTPUT_NOT_DEDICATED` | route | The output folder is the repository, its `build/` folder, or a folder that contains the repository. |
| `PACKAGE_OUTPUT_NOT_UNTRACKED` | route | The output folder is inside the repository but not ignored by Git. |
| `PACKAGE_PRIVATE_IDENTITY_PATH` | route | A retail title's package, which records local executable and module hashes, would be written inside the repository. |
| `PACKAGE_PSP_HEADER_REQUIRED` | route | The manifest takes BSS metadata from the PSP header, and no `--psp-header` was given. |
| `PACKAGE_REBUILD_NOT_INCOMPATIBLE` | player | Rebuild was asked for, but the package is not marked incompatible (it is missing or already usable). |
| `PACKAGE_REPORT_TOO_LARGE` | route | The package's `build-report.json` would exceed the size or structure limits of the package reader. |
| `PACKAGE_REUSE_INVALID` | route | The reusable generated-code source is missing, incomplete or fails its cache check, or the reuse options conflict. |
| `PACKAGE_REUSE_REFUSED` | route | The reusable package's cache key differs from this build's, so its generated C is not reused. |
| `PACKAGE_REVALIDATION_FAILED` | player | The installed package failed its check. |
| `PACKAGE_ROUTE_FAILED` | route | The package route stopped on a file-system or value error that carries no more specific route code. |
| `PACKAGE_UNSUPPORTED_PATH` | route | A path has spaces or characters the Make recipes cannot quote, and no short-name or `NK_BUILD_ROOT` alternative exists. |
| `RUNTIME_PACKAGE_NOT_READY` | player | The game's package has not been built yet. |

## run

| Code | Where | Meaning |
| --- | --- | --- |
| `DISPLAY_PROGRESS_UNVERIFIED` | report | The runtime's telemetry did not show whether the game set up its display. |
| `ENTRY_NOT_COMPILED` | report | The executable's entry has no generated function. |
| `EXITED_ZERO_BEFORE_FRAMEBUFFER_SETUP` | report | The runtime exited cleanly before the game set up its display. |
| `EXITED_ZERO_BEFORE_HLE` | report | The runtime exited cleanly before the game's first system call. |
| `EXPERIMENTAL_LAUNCH_UNAVAILABLE` | player | Launch now was asked for an experimental title; it is refused and the title is not added. |
| `GUEST_ACTIVITY_UNVERIFIED` | report | The runtime's telemetry did not show a system call. |
| `HEADLESS_UNAVAILABLE` | report | The run needs video output, and none is available. |
| `LAUNCH_DETAILS_UNAVAILABLE` | player | The player could not create the boot-event file it follows the launch with, so the game was not started. |
| `LAUNCH_FAILED` | report | The runtime exited with an error its output does not name, or could not be started. |
| `LAUNCH_TIMEOUT` | report | The launch did not finish within its time limit. |
| `LOOSE_CONTENT_ROOTS_UNAVAILABLE` | report | The manifest's loose-content folders could not be resolved against the title's data folder for the launch. |
| `MANIFEST_MISMATCH` | player | Launch preparation failed with an error about the title's manifest, profile or catalogue entry. |
| `MODULE_SELF_UNLOAD_BEFORE_FRAMEBUFFER_SETUP` | report | The game module unloaded itself before it set up its display. |
| `NATIVE_RUNTIME_CRASH` | report | The native runtime reported a crash. |
| `NO_FRAME_SUBMISSIONS` | report | No checked frame reached the presenter. |
| `PROCESS_SPAWN_FAILED` | player | The game could not be started: a last check before start refused it, or the operating system did not start the process. |
| `RUNTIME_ARGUMENT_FAILURE` | report | The runtime refused its launch arguments. |
| `RUNTIME_ELF_REJECTED` | report | The runtime refused the executable's load layout. |
| `RUNTIME_ERROR_EXIT` | player | The game's process exited with a non-zero code after its first 500 ms, without naming a stop boundary. |
| `RUNTIME_INPUT_UNAVAILABLE` | report | The runtime could not read a packaged input. |
| `RUNTIME_NOT_FOUND` | player | The game's recompiled program is not available. |
| `RUNTIME_NO_NEW_FRAMES` | player | The game presented no new frame for a long stretch, and it was stopped from the stall notice. |
| `RUNTIME_NO_PICTURE` | player | The game kept presenting frames, but every one was dark for a long stretch, and it was stopped from the stall notice. |
| `RUNTIME_PREMATURE_EXIT` | player | The game's process exited within 500 ms of starting, whatever its exit code. |
| `RUNTIME_SEMANTIC_BOUNDARY` | player | The game reached a feature Nakagawa does not run yet; the [stop boundary](#stop-boundaries) names it. |
| `RUNTIME_START_TIMEOUT` | player | The game's process wrote no boot event within the start time limit, so the player stopped it. |
| `RUNTIME_TRACE_UNAVAILABLE` | report | The runtime could not read a usable reference trace. |
| `RUN_BUDGET_ENDED_BEFORE_FRAMEBUFFER_SETUP` | report | The run's time budget ended while the game was still running, before it set up its display. |
| `STAGED_EXECUTABLE_INVALID` | player | The staged executable is missing, unreadable, or not a PSP executable the title manifest accepts. |
| `TITLE_DATA_MISSING` | player | The data folder the title manifest names was not found when the game was launched. |
| `TITLE_PROFILE_INVALID` | player | The title's settings file is invalid. |
| `UNRESOLVED_DISPATCH_TARGET` | report | The runtime refused a jump to code it has no translation for. |
| `UNSUPPORTED_IMPORT` | report | The game called a system function Nakagawa does not provide yet. |
| `UNSUPPORTED_INSTRUCTION` | report | The game ran an instruction the generated code and the interpreter do not cover. |
| `WINDOWS_BLOCKED_START` | player | Windows blocked the runtime from starting. |
| `WINDOWS_DEFENDER_THREAT` | player | Windows Security flagged the runtime and stopped it. |
| `WINDOWS_POLICY_BLOCKED` | player | A Windows policy blocked the runtime. |

## host

| Code | Where | Meaning |
| --- | --- | --- |
| `BUILD_TOOLCHAIN_MISSING` | player | The build tools are not installed. |
| `CACHE_CLEAR_FAILED` | player | Some files in the shared-library cache could not be removed. |
| `CANCELLED` | decrypt, disc, player | The operation was cancelled; nothing was kept. |
| `CLI_NOT_FOUND` | player | The bundled tools folder could not be found. |
| `DATA_DIR_UNAVAILABLE` | player | Nakagawa's data folder is not available. |
| `DESTINATION_WRITE_FAILED` | player | A verified download could not be moved into place, or its downloads folder could not be prepared. |
| `DISK_FULL` | player | The disk is full. |
| `DISK_WRITE_FAILED` | player | The Python runtime's staging folder could not be prepared, or its files could not be moved into app data. |
| `ERROR_UNKNOWN` | player | The player raised an error without a code. This is a defect in the player. |
| `FACTORY_RESET_BUSY` | player | Factory reset has to wait for a running operation. |
| `FACTORY_RESET_FAILED` | player | Factory reset did not finish. |
| `FOLDER_OPEN_FAILED` | player | A folder could not be opened. |
| `HASH_MISMATCH` | player | A download did not match its pinned SHA-256. |
| `HASH_UNAVAILABLE` | player | A download's hash could not be computed. |
| `INSTALL_CANCELLED` | player | The prerequisite install was cancelled. |
| `INTERNAL_ERROR` | decrypt, disc, player | A C engine returned a result it has no name for. This is a defect in Nakagawa. |
| `LIBRARY_WRITE_FAILED` | player | The library could not be saved. |
| `MANIFEST_INVALID` | player | A prerequisite's pinned download entry (URL, size or SHA-256) is invalid. |
| `OFFLINE_OR_NETWORK_ERROR` | player | A download failed because the network is unavailable. |
| `PATH_TOO_LONG` | player | A path is longer than the system allows. |
| `PLATFORM_UNSUPPORTED` | player | Prerequisite downloads and their checks run on Windows only. |
| `PREREQUISITE_BUNDLE_INVALID` | player | The bundled build tools are damaged. |
| `PREREQUISITE_FETCH_START_FAILED` | player | The prerequisite download could not start. |
| `PREREQUISITE_INSTALL_FAILED` | player | The prerequisite install failed. |
| `PREREQUISITE_MANIFEST_INVALID` | player | The prerequisite manifest is invalid. |
| `PYTHON_ARCHIVE_EXTRACT_FAILED` | player | The verified Python archive could not be extracted. |
| `PYTHON_ARCHIVE_INVALID` | player | The verified Python archive does not contain Python. |
| `PYTHON_BOOTSTRAP_FAILED` | player | The verified Python runtime could not be installed. |
| `PYTHON_BOOTSTRAP_START_FAILED` | player | The Python bootstrap could not start. |
| `PYTHON_NOT_FOUND` | player | The verified Python runtime is not in app data. |
| `REDIRECT_REJECTED` | player | A download was redirected to a host it does not use. |
| `REMOVE_FAILED` | player | The game could not be removed from the library. |
| `SIZE_MISMATCH` | player | A download is not the size its manifest lists. |
| `SPAWN_FAILED` | player | A tool process could not be started. |
| `SWEEP_REPORT_INVALID` | library | A sweep run's report has no failure class. |
| `SWEEP_ROUTE_FAILED` | library | A sweep run wrote no report. |
| `SWEEP_TIME_BUDGET` | library | A sweep run ended at its time budget. |
| `TITLE_TIME_BUDGET` | library | A library build ended at the title's time budget; `failed_stage` names the stage that was running. |
| `TOOLS_IN_USE` | player | The build tools are in use and cannot be removed now. |
| `TOOLS_REMOVE_FAILED` | player | The downloaded build tools could not be removed. |
| `TRUNCATED_BODY` | player | A download ended early. |
| `UPDATE_ANSWER_TOO_LARGE` | player | GitHub's answer to the update check was much larger than expected. |
| `UPDATE_ANSWER_UNEXPECTED` | player | GitHub's release list named a page or file outside this repository. |
| `UPDATE_ANSWER_UNREADABLE` | player | GitHub's answer was not a list of releases. |
| `UPDATE_CHECKSUM_CONFLICT` | player | The release's checksum file and GitHub's checksum disagree. |
| `UPDATE_CHECKSUM_INVALID` | player | The release's checksum file is not in the expected form. |
| `UPDATE_CHECKSUM_MISSING` | player | The release publishes no checksum for its Windows ZIP. |
| `UPDATE_DOWNLOAD_CANCELLED` | player | The update download was cancelled. |
| `UPDATE_DOWNLOAD_REMOVED` | player | The checked download is no longer in its folder. |
| `UPDATE_HASH_MISMATCH` | player | The download did not match the release's SHA-256. |
| `UPDATE_INCOMPLETE_DOWNLOAD` | player | The download is not the size the release lists. |
| `UPDATE_INSTALL_EXISTS` | player | The new version is already unpacked. |
| `UPDATE_INSTALL_INCOMPLETE` | player | Unpacking the new version did not finish. |
| `UPDATE_INSTALL_LEFTOVER` | player | A previous install attempt left files in the way. |
| `UPDATE_INSTALL_MOVE_FAILED` | player | The new version could not be moved into place. |
| `UPDATE_INSTALL_NOT_WRITABLE` | player | The install folder is not writable. |
| `UPDATE_INSTALL_PATH` | player | The folder for the new version has too long a path. |
| `UPDATE_NO_RELEASES` | player | GitHub lists no release yet. |
| `UPDATE_NO_WINDOWS_ZIP` | player | The release has no Windows ZIP. |
| `UPDATE_OFFLINE` | player | GitHub could not be reached. |
| `UPDATE_PLATFORM_UNSUPPORTED` | player | Checking for updates works on Windows only. |
| `UPDATE_REDIRECT_REJECTED` | player | GitHub redirected to a host the updater does not use. |
| `UPDATE_SAVE_FAILED` | player | The download could not be saved. |
| `UPDATE_START_FAILED` | player | An update step could not start. |
| `UPDATE_UNPACK_FAILED` | player | Windows' archive tool did not start. |
| `UPDATE_UNPACK_UNAVAILABLE` | player | Windows' archive tool was not found. |
| `UPDATE_VERSION_UNKNOWN` | player | This build carries no version to compare. |

## Stop boundaries

When a running game reaches something Nakagawa does not run yet, the runtime stops it at a
named boundary and the player's problem report says `RUNTIME_SEMANTIC_BOUNDARY`. The boundary
name is written as `BOOT_EVENT phase=stop reason=semantic-boundary boundary=<name>`.

| Boundary | Issue | Meaning |
| --- | --- | --- |
| `host-fiber-stack-bounds-unavailable` | 118 | The dispatcher could not measure the host fiber stack before a nested guest call. |
| `host-fiber-stack-exhausted` | 118 | A nested guest call would leave less than the 128 KiB safety margin on the host fiber stack. |
| `incomplete-executable-span` | 118 | An instruction fetch ran past the end of an executable span. |
| `interpreter-rejection` | 118 | The interpreter refused a transfer with no more specific name, or the boundary name was empty or invalid. |
| `invalid-control-flow` | 118 | The game made a control transfer the interpreter cannot follow. |
| `invalid-guest-memory-access` | 118 | The game accessed memory outside guest memory. |
| `libfont-startup-unavailable` | 299 | The game's libfont `module_start` could not run, so its font library never becomes ready. |
| `misaligned-control-target` | 118 | The game jumped to a misaligned address. |
| `misaligned-guest-memory-access` | 118 | The game made a misaligned memory access. |
| `target-not-executable` | 118 | The game jumped to an address outside executable code. |
| `unsupported-import-<name>` | 308 | The game called the system function `<name>`, which Nakagawa does not provide yet. |
| `unsupported-interpreter-form` | 118 | The interpreter met an instruction form it does not run. |

## Adding a code

1. Add it to [`assets/failure_codes.json`](../assets/failure_codes.json) with its stage and
   where it appears.
2. Add a row to the stage's table here.
3. For a bring-up report code, add it to the `failure_class` enum in
   [`assets/bringup_report.schema.json`](../assets/bringup_report.schema.json).
4. Run `python -m unittest tools.test_failure_codes`.
