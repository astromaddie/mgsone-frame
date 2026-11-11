#!/usr/bin/env python3
"""Relocate Frame absolute symlinks so the sysroot cannot escape its root."""

from __future__ import annotations

import argparse
import os
from collections import deque
from pathlib import Path, PurePosixPath


class SysrootError(RuntimeError):
    pass


def _resolve_inside(root: Path, relative_path: list[str]) -> Path:
    """Resolve symlinks with Frame absolute links interpreted inside root."""
    pending = deque(relative_path)
    resolved: list[str] = []
    followed = 0

    while pending:
        part = pending.popleft()
        if part in ("", "."):
            continue
        if part == "..":
            if not resolved:
                raise SysrootError("symlink escapes the sysroot")
            resolved.pop()
            continue

        candidate = root.joinpath(*resolved, part)
        if candidate.is_symlink():
            followed += 1
            if followed > 128:
                raise SysrootError(f"symlink loop while resolving {candidate}")
            target = os.readlink(candidate)
            target_parts = list(PurePosixPath(target).parts)
            if target.startswith("/"):
                resolved = []
                target_parts = target_parts[1:]
            pending.extendleft(reversed(target_parts))
            continue

        resolved.append(part)

    result = root.joinpath(*resolved)
    if not result.exists() and not result.is_symlink():
        raise SysrootError(f"symlink target is absent from the sysroot: {result}")
    return result


def normalize_sysroot(root_path: Path) -> int:
    root = root_path.resolve(strict=True)
    if not root.is_dir():
        raise SysrootError(f"not a sysroot directory: {root}")

    links: list[tuple[Path, str]] = []
    for directory, child_dirs, child_files in os.walk(root, followlinks=False):
        for name in [*child_dirs, *child_files]:
            link = Path(directory) / name
            if not link.is_symlink():
                continue
            raw = os.readlink(link)
            link_relative = list(link.relative_to(root).parts)
            if raw.startswith("/"):
                target = _resolve_inside(root, list(PurePosixPath(raw).parts[1:]))
                replacement = os.path.relpath(
                    target, link.parent
                )
                links.append((link, replacement))
            else:
                _resolve_inside(
                    root,
                    [*link_relative[:-1], *PurePosixPath(raw).parts],
                )

    for link, replacement in links:
        link.unlink()
        link.symlink_to(replacement)

    return len(links)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sysroot", type=Path, help="extracted Frame sysroot on D:")
    args = parser.parse_args()
    try:
        count = normalize_sysroot(args.sysroot)
    except (OSError, SysrootError) as exc:
        parser.error(str(exc))
    print(f"Normalized {count} absolute symlink(s); all targets stay inside {args.sysroot}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
