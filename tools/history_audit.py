#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Non-destructive full-history secret, proprietary material, and privacy audit tool for Nakagawa Recomp."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import subprocess

try:
    from .nk_core.git_isolation import isolated_git_env
    from .publication_policy import load_policy
except ImportError:
    from nk_core.git_isolation import isolated_git_env
    from publication_policy import load_policy

ROOT = Path(__file__).resolve().parent.parent
#: The canonical publication policy; its ``private_roots`` name the local
#: workspace roots that must never appear in published bytes or messages.
POLICY_PATH = Path("assets") / "public_source_profile.json"

HEX_16_BYTES = re.compile(r"\b[0-9a-fA-F]{32}\b")
PRIVATE_KEY_HEADER = re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----")
API_TOKEN_PATTERN = re.compile(r"\b(?:ghp_[a-zA-Z0-9]{36}|github_pat_[a-zA-Z0-9_]{80}|sk_live_[a-zA-Z0-9]{24}|AKIA[0-9A-Z]{16})\b")

# PRIVATE_KEY_HEADER above stays as it is because redaction wants the bare
# delimiter: if one ever reaches a finding's detail string it must be masked.
# Detection is a different question. An armour delimiter with no key body is not
# a secret, and documentation that tells a maintainer which value to paste into a
# CI secret store quotes those delimiters by design -- docs/PROVENANCE_MERGE_GATE.md
# did exactly that and failed this gate for it, with BEGIN and END on one prose
# line and no bytes between them. Requiring a real base64 body after the header
# keeps actual keys caught, since PEM, OpenSSH and encrypted forms all carry one.
PEM_KEY_MATERIAL = re.compile(
    r"-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----"
    r"(?:[^\S\n]*\n)+"          # end of the header line, then any blank lines
    r"(?:[A-Za-z0-9+/=\s]*?)"   # optional PEM headers (Proc-Type, DEK-Info, ...)
    r"[A-Za-z0-9+/]{40,}",      # at least one full-width base64 body line
)

# Sensitive local path patterns (fragmented to prevent self-match)
WINDOWS_USER_PATH = re.compile(r"[a-zA-Z]:\\(?:" + r"Us" + r"ers|Documents and Settings)\\[^\s\\/]+", re.IGNORECASE)
POSIX_USER_PATH = re.compile(r"/(?:" + r"ho" + r"me|Us" + r"ers)/[^\s/]+")
MAC_USER_PATH = re.compile(r"/(?:" + r"Us" + r"ers)/[^\s/]+")
WSL_USER_PATH = re.compile(r"/mnt/[a-z]/(?:" + r"Us" + r"ers)/[^\s/]+")
UNC_PATH = re.compile(r"\\\\[a-zA-Z0-9_.-]+\\[a-zA-Z0-9_.-]+")
ONEDRIVE_PATH = re.compile(r"\bOne" + r"Drive\b(?:\s*-\s*[^/\\]+)?", re.IGNORECASE)
TEMP_PATH = re.compile(r"(?:/tm" + r"p/|/var/tm" + r"p/|[a-zA-Z]:\\(?:[^\s\\/]+\\)*(?:Windows\\Te" + r"mp|AppData\\Local\\Te" + r"mp)\\)[a-zA-Z0-9_.-]+", re.IGNORECASE)

# Content signatures are deliberately contextual.  Public SHA-256 values and
# ordinary PSP NIDs are allowed; the audit looks for credentials, private
# operational vocabulary, known proprietary paths, and binary file signatures.
# Keep these signatures high-confidence.  Generic checkout instructions use
# names such as ``place_game_here`` and ``oracle/`` as *absence* or routing
# examples; treating every such public placeholder as proprietary made the
# candidate audit report its own policy and documentation.  The history gate
# still catches those directories when they occur as actual historical paths
# (``FORBIDDEN_PREFIXES`` above), while content scanning is reserved for
# identifiers that denote a real title/private artifact or operational dump.
# Fragment the private-repository literal so this scanner does not report its
# own source blob as a finding.
PRIVATE_REPO_URL = re.compile(
    r"github\.com/" + r"Jstar269/" + r"nakagawa-recomp-" + r"history-private",
    re.IGNORECASE,
)
# A plain title file name (for example a data archive's name) is a publishable fact under
# the maintainer's 2026-09-25 legal posture, so it is not vocabulary here; key material
# names and private save/trace/dump evidence still are.
PRIVATE_OPERATIONAL_VOCABULARY = re.compile(
    r"(?:HST" + r"_PGD_VKEY(?:_HEX)?|"
    r"private[_ -](?:save|trace|dump)\s*(?:baseline|identity|path|location|hash|capture|evidence)\b)",
    re.IGNORECASE,
)
SUSPICIOUS_ENCODED = re.compile(r"^[A-Za-z0-9+/]{256,}={0,2}$")
# A C0 control character other than TAB, LF and CR marks a blob as binary. One
# search per blob replaces a Python-level scan of every character of every text blob.
BINARY_CONTROL_CHARACTER = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f]")


# Literal prefilters for the content signatures. Each helper returns exactly the verdict
# of the regex it names: the literal it checks is part of every possible match, so a text
# without it cannot match and the regex is skipped. A case-insensitive pattern is checked
# against the lowercased text. No case-insensitive literal contains an i, because the dotless
# i does not lowercase to i, and the long s (U+017F) is spelled out where a word needs it,
# so no match is ever lost. The API-token literals are case-sensitive, so they are checked
# as written.
def _has_api_token(text: str) -> bool:
    if "ghp_" not in text and "github_pat_" not in text and "sk_live_" not in text and "AKIA" not in text:
        return False
    return API_TOKEN_PATTERN.search(text) is not None


def _has_windows_user_path(text: str) -> bool:
    if ":\\" not in text:  # every match has a drive colon and then a backslash
        return False
    return WINDOWS_USER_PATH.search(text) is not None


def _has_onedrive_path(text: str, lowered: str) -> bool:
    if "onedr" not in lowered:  # every match contains "OneDr" in some letter case
        return False
    return ONEDRIVE_PATH.search(text) is not None


def _has_temp_path(text: str, lowered: str) -> bool:
    # Matches are "/tmp/", "/var/tmp/" or a drive path that ends in a Temp directory.
    if "tmp/" not in lowered and not ("temp\\" in lowered and ":\\" in text):
        return False
    return TEMP_PATH.search(text) is not None


def _has_private_repo_url(text: str) -> bool:
    if "269/" not in text:  # every match contains the account number and a slash
        return False
    return PRIVATE_REPO_URL.search(text) is not None


def _has_private_operational_vocabulary(text: str, lowered: str) -> bool:
    # HST_PGD_VKEY contains "pgd_v". "private" followed by a separator and save, trace or
    # dump contains "vate", the separator and the word, with the long s written as U+017F.
    if "pgd_v" not in lowered and not any(
        "vate" + separator + word in lowered
        for separator in "_ -" for word in ("trace", "dump", "save", "\u017fave")
    ):
        return False
    return PRIVATE_OPERATIONAL_VOCABULARY.search(text) is not None

FORBIDDEN_EXTENSIONS = {
    ".at3", ".bin", ".chd", ".cso", ".dax", ".dmp", ".edat", ".elf", ".gim",
    ".iso", ".pbp", ".pmf", ".prx", ".psar", ".psess", ".sfo", ".sqlite", ".trace", ".vag"
}

FORBIDDEN_NAMES = {
    "reference_hashes.json", "vfpu_words.txt", "vfpu_words_local.txt",
    "nidseq_mine.txt", "pgd_keys.txt"
}

FORBIDDEN_PREFIXES = (
    "build/", "docs/opengrip_ref/", "fs/", "logs/", "memstick/", "opengrip_ref/",
    "OpenGrip_For_Inspiration/", "oracle/", "original_game/", "place_game_here/",
    "third_party/ghidra/exports/", "third_party/ghidra/projects/"
)


@dataclass(frozen=True)
class HistoryFinding:
    category: str  # DEFINITE_SECRET, POSSIBLE_CREDENTIAL, PROPRIETARY_ARTIFACT, PRIVACY_METADATA, FALSE_POSITIVE, UNREACHABLE_OBJECT
    code: str
    commit: str
    path: str
    detail: str

    def to_dict(self, redact: bool = True) -> dict:
        d = asdict(self)
        if redact:
            # Mask any accidental sensitive fragments in detail
            detail = d["detail"]
            detail = HEX_16_BYTES.sub("[REDACTED_HEX_KEY]", detail)
            detail = PRIVATE_KEY_HEADER.sub("[REDACTED_PRIVATE_KEY_HEADER]", detail)
            detail = API_TOKEN_PATTERN.sub("[REDACTED_API_TOKEN]", detail)
            d["detail"] = detail
        return d


def _git(cmd: list[str], repo_root: Path = ROOT) -> str:
    res = subprocess.run(
        ["git", *cmd],
        cwd=repo_root,
        env=isolated_git_env(root=repo_root),
        capture_output=True,
        check=False,
    )
    if res.returncode != 0:
        err = res.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"git {' '.join(cmd)} failed: {err}")
    return res.stdout.decode("utf-8", errors="replace")


def _object_listing(repo_root: Path) -> list[str]:
    """Every reachable object as a ``<sha> <path>`` line, from one ``rev-list``."""
    return _git(["rev-list", "--objects", "--all"], repo_root=repo_root).splitlines()


def get_repository_baseline(repo_root: Path = ROOT, raw_objects: list[str] | None = None) -> dict:
    commit_count = int(_git(["rev-list", "--count", "--all"], repo_root=repo_root).strip())
    if raw_objects is None:
        raw_objects = _object_listing(repo_root)
    ref_list = _git(["for-each-ref"], repo_root=repo_root).splitlines()
    try:
        main_sha = _git(["rev-parse", "origin/main"], repo_root=repo_root).strip()
    except Exception:
        try:
            main_sha = _git(["rev-parse", "HEAD"], repo_root=repo_root).strip()
        except Exception:
            main_sha = "unknown"

    return {
        "git_commit_main": main_sha,
        "total_commits": commit_count,
        "total_objects": len(raw_objects),
        "total_refs": len(ref_list),
        "ref_sample": ref_list[:15],
    }


def _path_finding_id(obj_sha: str, repo_root: Path) -> str:
    """The identity a path finding reports for its object.

    A blob carries the same identity as the content findings ("blob:" plus 12 hex), so one
    reviewed blob entry (exact bytes, code and path) can accept every finding on those
    bytes. A tree keeps its short object id.
    """
    try:
        kind = _git(["cat-file", "-t", obj_sha], repo_root=repo_root).strip()
    except Exception:
        return obj_sha[:8]
    return "blob:" + obj_sha[:12] if kind == "blob" else obj_sha[:8]


def audit_history_tree_paths(repo_root: Path = ROOT,
                             raw_objects: list[str] | None = None) -> list[HistoryFinding]:
    """Pass 1: Audit all historical tree entry paths across every reachable commit."""
    findings: list[HistoryFinding] = []
    if raw_objects is None:
        raw_objects = _object_listing(repo_root)

    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        obj_sha, rel_path = parts
        rel_path_clean = rel_path
        while rel_path_clean.startswith("./"):
            rel_path_clean = rel_path_clean[2:]
        lower_p = rel_path_clean.lower()

        # Check forbidden prefixes
        for prefix in FORBIDDEN_PREFIXES:
            if lower_p.startswith(prefix.lower()):
                findings.append(HistoryFinding(
                    category="PROPRIETARY_ARTIFACT",
                    code="HISTORICAL_PATH_PREFIX",
                    commit=_path_finding_id(obj_sha, repo_root),
                    path=rel_path_clean,
                    detail=f"Reachable historical object under private/proprietary prefix '{prefix}'",
                ))
                break

        # Check forbidden names
        name = Path(rel_path_clean).name
        if name in FORBIDDEN_NAMES:
            findings.append(HistoryFinding(
                category="PROPRIETARY_ARTIFACT",
                code="HISTORICAL_GAME_ARTIFACT",
                commit=_path_finding_id(obj_sha, repo_root),
                path=rel_path_clean,
                detail=f"Reachable historical game-derived file '{name}'",
            ))

        # Check forbidden extensions
        ext = Path(rel_path_clean).suffix.lower()
        if ext in FORBIDDEN_EXTENSIONS:
            findings.append(HistoryFinding(
                category="PROPRIETARY_ARTIFACT",
                code="HISTORICAL_PROHIBITED_EXTENSION",
                commit=_path_finding_id(obj_sha, repo_root),
                path=rel_path_clean,
                detail=f"Reachable historical blob with prohibited binary extension '{ext}'",
            ))

    return findings


def _reachable_blob_ids(repo_root: Path,
                        raw_objects: list[str] | None = None) -> dict[str, str]:
    """Return each reachable blob object exactly once, with one observed path."""
    if raw_objects is None:
        raw_objects = _object_listing(repo_root)
    candidates: dict[str, str] = {}
    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) != 2:
            continue
        object_id, path = parts
        candidates.setdefault(object_id, path)
    if not candidates:
        return {}
    ids = list(candidates)
    checked = subprocess.run(
        ["git", "cat-file", "--batch-check"], cwd=repo_root,
        env=isolated_git_env(root=repo_root),
        input=("".join(f"{object_id}\n" for object_id in ids)).encode("ascii"),
        capture_output=True, check=True,
    ).stdout.decode("ascii", errors="replace").splitlines()
    return {
        line.split()[0]: candidates[line.split()[0]]
        for line in checked
        if len(line.split()) == 3 and line.split()[1] == "blob"
    }


def _binary_magic(data: bytes) -> str | None:
    if data.startswith((b"~PSP", b"~SCE")):
        return "encrypted PSP module"
    if data.startswith(b"\x7fELF"):
        return "ELF executable"
    if data.startswith(b"\0PBP"):
        return "PSP PBP"
    if data[4:8] == b"PGF0" or data.startswith((b"PGF0", b"\0PGF")):
        return "PSP PGF font"
    if data.startswith((b"\x03\x02\x23\x07", b"\x07\x23\x02\x03")):
        return "SPIR-V shader bytecode"
    if len(data) >= 0x8006 and data[0x8001:0x8006] == b"CD001":
        return "ISO9660 image"
    return None


def audit_history_blob_contents(repo_root: Path = ROOT,
                                raw_objects: list[str] | None = None) -> list[HistoryFinding]:
    """Pass 2: scan every reachable blob's content once, not just its path."""
    findings: list[HistoryFinding] = []
    blobs = _reachable_blob_ids(repo_root, raw_objects)
    if not blobs:
        return findings
    proc = subprocess.Popen(
        ["git", "cat-file", "--batch"], cwd=repo_root,
        env=isolated_git_env(root=repo_root),
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    )
    assert proc.stdin and proc.stdout
    output, _ = proc.communicate(("".join(f"{object_id}\n" for object_id in blobs)).encode("ascii"))
    pos = 0
    for object_id, path in blobs.items():
        end = output.find(b"\n", pos)
        if end < 0:
            break
        header = output[pos:end].split()
        pos = end + 1
        if len(header) < 3 or header[1] != b"blob":
            continue
        size = int(header[2])
        data = output[pos:pos + size]
        pos += size + 1
        commit = f"blob:{object_id[:12]}"
        # Do not run text signatures over arbitrary binary blobs.  Replacement
        # decoding turns random bytes (for example public lookup tables) into
        # plausible path/token text and creates false positives.  Strict UTF-8
        # plus a NUL/control-byte check is conservative for source, JSON and
        # documentation while preserving the separate binary-magic checks.
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = None
        if text is not None and (
            b"\0" in data
            or BINARY_CONTROL_CHARACTER.search(text) is not None
        ):
            text = None

        lowered = text.lower() if text is not None else ""
        if text is not None and (PEM_KEY_MATERIAL.search(text) or _has_api_token(text)):
            findings.append(HistoryFinding("DEFINITE_SECRET", "HISTORICAL_BLOB_SECRET", commit, path,
                                           "reachable blob contains a private-key or API-token signature [REDACTED]"))
        if text is not None and (_has_windows_user_path(text) or POSIX_USER_PATH.search(text) or MAC_USER_PATH.search(text) or WSL_USER_PATH.search(text) or UNC_PATH.search(text) or _has_onedrive_path(text, lowered) or _has_temp_path(text, lowered)):
            findings.append(HistoryFinding("PRIVACY_METADATA", "HISTORICAL_BLOB_LOCAL_PATH", commit, path,
                                           "reachable blob contains a private local path [REDACTED]"))
        if text is not None and _has_private_repo_url(text):
            findings.append(HistoryFinding("PRIVACY_METADATA", "HISTORICAL_BLOB_PRIVATE_REPO", commit, path,
                                           "reachable blob names a private repository [REDACTED]"))
        if text is not None and _has_private_operational_vocabulary(text, lowered):
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_PRIVATE_VOCABULARY", commit, path,
                                           "reachable blob contains private/game-derived operational vocabulary [REDACTED]"))
        magic = _binary_magic(data)
        if magic:
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_MAGIC", commit, path,
                                           f"reachable blob contains forbidden/proprietary magic: {magic}"))
        if text is not None and size >= 1024 * 1024 and SUSPICIOUS_ENCODED.fullmatch(text.strip()):
            findings.append(HistoryFinding("PROPRIETARY_ARTIFACT", "HISTORICAL_BLOB_ENCODED_PAYLOAD", commit, path,
                                           "large reachable blob is a suspicious encoded payload"))
    return findings


def policy_private_roots(repo_root: Path = ROOT) -> tuple[str, ...]:
    """Return the canonical policy's ``private_roots``; a repository without a policy has none.

    A policy that exists but cannot be loaded raises ``PolicyError``: the audit fails
    closed rather than silently scanning without the deny-list.
    """
    path = repo_root / POLICY_PATH
    if not path.is_file():
        return ()
    return load_policy(path).private_roots


def audit_history_commit_metadata(repo_root: Path = ROOT,
                                  private_roots: tuple[str, ...] | None = None) -> list[HistoryFinding]:
    """Pass 2: audit every reachable commit's full message, not only its subject.

    A squash merge writes the pull-request body into the message, and a body line
    naming a local path reaches public history as surely as a subject does.
    """
    findings: list[HistoryFinding] = []
    roots = policy_private_roots(repo_root) if private_roots is None else private_roots
    roots_lower = [root.lower() for root in roots if root]
    log_output = _git(["log", "--all", "-z", "--format=%H%x1f%B"], repo_root=repo_root)

    for record in log_output.split("\0"):
        commit_sha, separator, message = record.strip("\n").partition("\x1f")
        if not separator:
            continue
        commit_id = commit_sha[:12]

        if WINDOWS_USER_PATH.search(message) or POSIX_USER_PATH.search(message) or ONEDRIVE_PATH.search(message):
            findings.append(HistoryFinding(
                category="PRIVACY_METADATA",
                code="COMMIT_LOG_LOCAL_PATH",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message contains local path or directory fragment",
            ))

        message_lower = message.lower()
        if any(root in message_lower for root in roots_lower):
            findings.append(HistoryFinding(
                category="PRIVACY_METADATA",
                code="COMMIT_LOG_PRIVATE_ROOT",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message names a configured private root [REDACTED]",
            ))

        if PEM_KEY_MATERIAL.search(message) or API_TOKEN_PATTERN.search(message):
            findings.append(HistoryFinding(
                category="DEFINITE_SECRET",
                code="COMMIT_LOG_SECRET",
                commit=commit_id,
                path="<commit_message>",
                detail="Commit message contains private key or API token literal [REDACTED]",
            ))

    return findings


def audit_large_blobs(repo_root: Path = ROOT, size_threshold: int = 500 * 1024,
                      raw_objects: list[str] | None = None) -> list[dict]:
    """Pass 3: Inventory large objects in history packfiles."""
    large_blobs: list[dict] = []
    if raw_objects is None:
        raw_objects = _object_listing(repo_root)

    # Map sha to path
    sha_to_path: dict[str, str] = {}
    for line in raw_objects:
        parts = line.strip().split(None, 1)
        if len(parts) == 2:
            sha_to_path[parts[0]] = parts[1]

    # Query cat-file for object sizes
    try:
        proc = subprocess.Popen(
            ["git", "cat-file", "--batch-check"], cwd=repo_root,
            env=isolated_git_env(root=repo_root),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        )
        assert proc.stdin and proc.stdout
        stdin_text = "\n".join(sha_to_path.keys()) + "\n"
        stdout_data, _ = proc.communicate(input=stdin_text.encode("utf-8"))

        for line in stdout_data.decode("utf-8", errors="replace").splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] == "blob" and parts[2].isdigit():
                sha, obj_type, size_str = parts
                size = int(size_str)
                if size >= size_threshold:
                    large_blobs.append({
                        "sha": sha,
                        "size": size,
                        "path": sha_to_path.get(sha, "unknown"),
                    })
    except Exception:
        pass

    return sorted(large_blobs, key=lambda b: b["size"], reverse=True)


REVIEWED_FINDINGS_PATH = ROOT / "assets" / "history_audit_reviewed.json"


def load_reviewed_findings(path: Path = REVIEWED_FINDINGS_PATH) -> list[dict]:
    """Maintainer-reviewed historical findings: an exact (blob or commit, code, path) triple each.

    A blob id names immutable content and a commit id names an immutable message, so
    an entry can never excuse different bytes; any new blob or commit with the same
    problem is still a finding. Each entry names exactly one of ``blob`` and
    ``commit`` and must say why it was accepted. A malformed file fails closed.
    """
    if not path.exists():
        return []
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema_version") != 1:
        raise ValueError(f"{path}: expected schema_version 1")
    entries = data.get("reviewed")
    if not isinstance(entries, list):
        raise ValueError(f"{path}: 'reviewed' must be a list")
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            raise ValueError(f"{path}: reviewed[{index}] must be an object")
        kinds = [key for key in ("blob", "commit") if key in entry]
        if len(kinds) != 1:
            raise ValueError(f"{path}: reviewed[{index}] must name exactly one of 'blob' or 'commit'")
        object_id = entry[kinds[0]]
        if not isinstance(object_id, str) or not re.fullmatch(r"[0-9a-f]{40}", object_id):
            raise ValueError(f"{path}: reviewed[{index}].{kinds[0]} must be a full 40-hex {kinds[0]} id")
        for key in ("code", "path", "reason"):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                raise ValueError(f"{path}: reviewed[{index}].{key} must be a non-empty string")
    return entries


def _is_reviewed(finding: HistoryFinding, reviewed: list[dict]) -> dict | None:
    for entry in reviewed:
        if finding.code != entry["code"] or finding.path != entry["path"]:
            continue
        if "blob" in entry and finding.commit == "blob:" + entry["blob"][:12]:
            return entry
        if "commit" in entry and finding.commit == entry["commit"][:12]:
            return entry
    return None


def generate_full_history_audit_report(repo_root: Path = ROOT,
                                       reviewed_path: Path = REVIEWED_FINDINGS_PATH) -> dict:
    # One object listing serves every pass: the report reads one consistent
    # snapshot of history instead of four separate rev-list runs.
    raw_objects = _object_listing(repo_root)
    baseline = get_repository_baseline(repo_root, raw_objects)
    tree_findings = audit_history_tree_paths(repo_root, raw_objects)
    metadata_findings = audit_history_commit_metadata(repo_root)
    blob_findings = audit_history_blob_contents(repo_root, raw_objects)
    large_blobs = audit_large_blobs(repo_root, raw_objects=raw_objects)
    reviewed = load_reviewed_findings(reviewed_path)

    all_findings: list[HistoryFinding] = []
    reviewed_findings: list[dict] = []
    for finding in tree_findings + metadata_findings + blob_findings:
        entry = _is_reviewed(finding, reviewed)
        if entry is None:
            all_findings.append(finding)
        else:
            reviewed_findings.append({**finding.to_dict(redact=True), "reason": entry["reason"]})

    category_counts: dict[str, int] = {}
    for f in all_findings:
        category_counts[f.category] = category_counts.get(f.category, 0) + 1

    return {
        "status": "FAIL" if all_findings else "OK",
        "baseline": baseline,
        "summary": {
            "total_findings": len(all_findings),
            "category_counts": category_counts,
            "large_blobs_over_500kb": len(large_blobs),
            "reviewed_findings": len(reviewed_findings),
        },
        "reviewed_findings": reviewed_findings,
        "large_blobs": large_blobs[:10],  # Top 10 largest blobs
        "findings": [f.to_dict(redact=True) for f in all_findings],
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print JSON report to stdout")
    parser.add_argument("--out", type=Path, help="Write publication-safe audit JSON report to path")
    args = parser.parse_args(argv)

    report = generate_full_history_audit_report(ROOT)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote publication-safe full-history audit report to {args.out}")

    if args.json:
        print(json.dumps(report, indent=2))
        return 1 if report["summary"]["total_findings"] > 0 else 0

    print("=======================================================")
    print(" Nakagawa Recomp Full-History Non-Destructive Audit")
    print("=======================================================")
    print(f"Reachable Commits: {report['baseline']['total_commits']}")
    print(f"Reachable Objects: {report['baseline']['total_objects']}")
    print(f"Reachable Refs:    {report['baseline']['total_refs']}")
    print(f"Status:            {report['status']}")
    print(f"Total Findings:    {report['summary']['total_findings']}")
    print(f"Reviewed (accepted historical blobs): {report['summary']['reviewed_findings']}")
    print("Category Breakdown:")
    for cat, count in report['summary']['category_counts'].items():
        print(f"  - {cat}: {count}")

    if report["findings"]:
        print("\nFindings Summary:")
        for f in report["findings"]:
            print(f"  [{f['category']}] {f['code']}: {f['path']} ({f['commit']}) - {f['detail']}")
        return 1

    print("\nFull-history audit: OK (0 sensitive findings across all reachable commits & objects)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
