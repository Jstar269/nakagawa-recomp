# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

from __future__ import annotations

from collections.abc import Mapping, Sequence
import functools
import os
from pathlib import Path
import subprocess


_REPOSITORY_SELECTING_VARIABLES = frozenset({
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_CEILING_DIRECTORIES",
    "GIT_COMMON_DIR",
    "GIT_DIR",
    "GIT_DISCOVERY_ACROSS_FILESYSTEM",
    "GIT_INDEX_FILE",
    "GIT_NAMESPACE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_PREFIX",
    "GIT_QUARANTINE_PATH",
    "GIT_SHALLOW_FILE",
    "GIT_WORK_TREE",
})


@functools.lru_cache(maxsize=32)
def _absolute_git_dir(root: str) -> str | None:
    """The repository directory `root` resolves to with no inherited selectors."""
    env = {key: value for key, value in os.environ.items()
           if key not in _REPOSITORY_SELECTING_VARIABLES and not key.startswith("GIT_CONFIG")}
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--absolute-git-dir"],
            cwd=root, env=env, capture_output=True, text=True, check=False,
        )
    except OSError:
        return None
    if result.returncode != 0 or not result.stdout.strip():
        return None
    return os.path.normcase(os.path.realpath(result.stdout.strip()))


def _index_file_inside(root: Path, index_file: str) -> str | None:
    """`index_file` as an absolute path when it belongs to root's own repository.

    A pre-commit hook runs with GIT_INDEX_FILE naming the index being committed
    (a lock file for `git commit -a` or a path-limited commit). Audits of the real
    checkout must read that index, while an inherited index that points into some
    other repository is still dropped.
    """
    git_dir = _absolute_git_dir(str(root))
    if git_dir is None:
        return None
    candidate = Path(index_file)
    if not candidate.is_absolute():
        candidate = root / candidate
    resolved = os.path.normcase(os.path.realpath(candidate))
    if os.path.dirname(resolved) != git_dir:
        return None
    return str(candidate)


def isolated_git_env(
    base: Mapping[str, str] | None = None,
    *,
    root: str | os.PathLike[str] | None = None,
) -> dict[str, str]:
    env = dict(os.environ if base is None else base)
    kept_index = None
    if root is not None and env.get("GIT_INDEX_FILE"):
        kept_index = _index_file_inside(Path(root).resolve(), env["GIT_INDEX_FILE"])
    for key in tuple(env):
        if key in _REPOSITORY_SELECTING_VARIABLES or key.startswith("GIT_CONFIG"):
            del env[key]
    env.update({
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_AUTHOR_NAME": "Nakagawa Recomp Tests",
        "GIT_AUTHOR_EMAIL": "tests@nakagawa-recomp.invalid",
        "GIT_COMMITTER_NAME": "Nakagawa Recomp Tests",
        "GIT_COMMITTER_EMAIL": "tests@nakagawa-recomp.invalid",
    })
    if root is not None:
        env["GIT_CEILING_DIRECTORIES"] = str(Path(root).resolve().parent)
    if kept_index is not None:
        env["GIT_INDEX_FILE"] = kept_index
    return env


def run_git(
    args: Sequence[str],
    cwd: str | os.PathLike[str],
    *,
    env: Mapping[str, str] | None = None,
    **kwargs,
):
    return subprocess.run(
        ["git", *args],
        cwd=cwd,
        env=isolated_git_env(env, root=cwd),
        **kwargs,
    )
