#!/usr/bin/env python3
"""Create a clean, self-contained GitHub project tree."""

from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path


FILES = (
    "Makefile",
    "config.example.ini",
    "LICENSE",
    "LICENSES/THIRD_PARTY.md",
    ".github/workflows/build.yml",
    "tools/embed_assets.py",
    "tools/package_release.py",
    "tools/prepare_github_project.py",
    "docs/github/README.md",
    "docs/github/CHANGELOG.md",
    "docs/github/gitignore",
)
TREES = ("src", "web", "tests", "third_party")


def copy_file(root: Path, output: Path, relative: str) -> None:
    source = root / relative
    destination = output / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def prepare(root: Path, output: Path) -> None:
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for relative in FILES:
        copy_file(root, output, relative)
    for tree in TREES:
        shutil.copytree(
            root / tree,
            output / tree,
            ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".DS_Store"),
        )
    shutil.copy2(root / "docs/github/README.md", output / "README.md")
    shutil.copy2(root / "docs/github/CHANGELOG.md", output / "CHANGELOG.md")
    shutil.copy2(root / "docs/github/gitignore", output / ".gitignore")
    elf = root / "ps5localsend.elf"
    shutil.copy2(elf, output / elf.name)
    digest = hashlib.sha256(elf.read_bytes()).hexdigest()
    (output / "SHA256SUMS").write_text(
        f"{digest}  ps5localsend.elf\n", encoding="ascii"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root.resolve(), args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
