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
def _absolute_git_dir(cwd: str, git_dir: str | None = None) -> str | None:
    """The repository directory git resolves from `cwd` (and an explicit GIT_DIR)."""
    env = {key: value for key, value in os.environ.items()
           if key not in _REPOSITORY_SELECTING_VARIABLES and not key.startswith("GIT_CONFIG")}
    if git_dir:
        env["GIT_DIR"] = git_dir
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--absolute-git-dir"],
            cwd=cwd, env=env, capture_output=True, text=True, check=False,
        )
    except OSError:
        return None
    if result.returncode != 0 or not result.stdout.strip():
        return None
    return os.path.normcase(os.path.realpath(result.stdout.strip()))


def _caller_index_for(root: Path, env: Mapping[str, str]) -> str | None:
    """The caller's GIT_INDEX_FILE, as an absolute path, when it indexes `root`.

    A pre-commit hook runs with GIT_INDEX_FILE naming the index being committed (a
    lock file for `git commit -a` or a path-limited commit), and the publication
    tests stage candidates in a temporary index the same way. When the caller's own
    repository (its GIT_DIR, else its working directory) is the repository being
    inspected, that index is the caller's intent and is kept. An index inherited
    while inspecting some other repository, such as a scratch repository created by
    a test, is dropped.
    """
    target = _absolute_git_dir(str(root))
    if target is None:
        return None
    caller = _absolute_git_dir(os.getcwd(), env.get("GIT_DIR"))
    if caller != target:
        return None
    index_file = Path(env["GIT_INDEX_FILE"])
    if not index_file.is_absolute():
        index_file = Path.cwd() / index_file
    return str(index_file)


def isolated_git_env(
    base: Mapping[str, str] | None = None,
    *,
    root: str | os.PathLike[str] | None = None,
) -> dict[str, str]:
    env = dict(os.environ if base is None else base)
    kept_index = None
    if root is not None and env.get("GIT_INDEX_FILE"):
        kept_index = _caller_index_for(Path(root).resolve(), env)
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
