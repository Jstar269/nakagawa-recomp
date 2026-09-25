# Archive-backed read-only VFS

Issue #298 implements the generic archive side of the title data route. A title
configuration supplies the data root; this layer does not know a title name, a
guest address, or a fixed archive filename.

## Contract

`SrArchiveVfs` owns one or more validated `NkXbArchive` mounts. Mount paths are
archive paths supplied by the caller, and the provider derives a numeric XB
variant from a `.xbN` suffix when requested. Every archive is opened through
`nk_xb_open_file` or `nk_xb_open_memory`, so signature, bounds, name, duplicate,
size, compression-header, and path validation remain owned by `nk_xb`.

The provider builds one sorted metadata index from archive members. It never
extracts a member and never performs a recursive walk of an extracted tree.
Lookups use the normalized, ASCII-folded member key. A qualified guest key
selects its exact numeric variant; an unqualified key prefers an unqualified
member and then the lowest numeric variant. Mount order is the deterministic
order supplied by the caller; the first member wins a same-key collision.

The HLE data route checks a loose file at the configured data root before
consulting the archive index. A directory listing merges loose children first,
then archive children, using the existing first-source-wins VFS merge rule.
This makes a user override win for both `sceIoOpen` and `sceIoDread` without
inventing a path alias. Archive lookup is limited to `disc0:`/`umd:` and
unqualified relative keys; it never shadows `ms0:`/`fatms0:` storage. A
malformed archive aborts preparation; a missing member is not an error. The
provider never creates, writes, or repacks files.

## Read path and cache

An open descriptor records the mount and member indices. Reads validate the
requested range, decode through the existing `nk_xb_read_entry` boundary, and
copy only the requested range. Uncompressed ranges can be copied directly;
compressed members are decoded on demand. Decoded members use a bounded cache
with a byte budget and entry-count budget. A member larger than the cache is
decoded into a temporary bounded buffer for that read and is not retained.

Seek, partial reads, `sceIoGetstat`, and `sceIoDread` use the same indexed
metadata as reads. A decoder or cache failure returns an I/O failure; it never
returns fabricated bytes or silently falls through to an unrelated source.

## Startup boundary

`sr_host_data_prepare` discovers archive files in the declared data root before
guest execution. If at least one archive is present, it builds the archive
index and skips the extracted-member walk. Loose files remain available through
bounded on-demand path checks and per-directory enumeration. If no archive is
present, the existing loose-file census and lookup path is used unchanged.

A data root with no archives therefore retains the old behavior. A data root
with an invalid archive is `FAILED`, even if a loose tree exists, so malformed
container data cannot be hidden by an unrelated source.

## Boundaries and follow-up

The provider is read-only. Archive selection, localized roots, title directory
layout, and the relationship between a packed `xbdata` directory and adjacent
loose content belong to the manifest/filesystem configuration coordinated with
issue #289. Private-title acceptance, hardware behavior, archive repacking, and
write-through overlays are outside this issue.
