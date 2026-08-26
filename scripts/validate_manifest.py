#!/usr/bin/env python3
"""Local manifest validator for nakagawa-recomp.

Checks basic Scoop manifest requirements without external dependencies.
Usage: python scripts/validate_manifest.py [path-or-dir...]
If no arguments are provided, checks all .json files recursively.
"""
import json
import sys
from pathlib import Path


def check_manifest(path: Path):
    errors = []
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except Exception as e:
        return [f"{path}: invalid JSON: {e}"]
    if not isinstance(data, dict):
        return [f"{path}: top-level JSON is not an object"]
    # Required top-level fields
    for field in ("version",):
        if field not in data:
            errors.append(f"{path}: missing required '{field}' field")
    if 'architecture' not in data:
        errors.append(f"{path}: missing required 'architecture' field")
        return errors
    arch = data['architecture']
    if not isinstance(arch, dict):
        errors.append(f"{path}: 'architecture' must be an object")
        return errors
    for a in ('64bit','32bit'):
        if a in arch:
            ent = arch[a]
            if not isinstance(ent, dict):
                errors.append(f"{path}: architecture.{a} must be an object")
                continue
            if 'url' not in ent:
                errors.append(f"{path}: architecture.{a} missing 'url'")
            if 'hash' not in ent and not ('hash' in data or 'autoupdate' in data):
                errors.append(f"{path}: architecture.{a} missing 'hash'")
            # bin is recommended if package supplies executables
    # Basic autoupdate/checkver shape checks
    if 'autoupdate' in data:
        au = data['autoupdate']
        if not isinstance(au, dict):
            errors.append(f"{path}: 'autoupdate' must be an object")
    return errors


def main():
    paths = sys.argv[1:] or ["."]
    files = []
    for p in paths:
        pth = Path(p)
        if pth.is_file() and pth.suffix.lower() == '.json':
            files.append(pth)
        elif pth.is_dir():
            files.extend(sorted(pth.rglob('*.json')))
    if not files:
        print('No JSON files found to validate.')
        return
    all_errors = []
    for f in files:
        errs = check_manifest(f)
        if errs:
            all_errors.extend(errs)
    if all_errors:
        print('\n'.join(all_errors))
        sys.exit(1)
    print('All manifests passed local validation')

if __name__ == '__main__':
    main()
