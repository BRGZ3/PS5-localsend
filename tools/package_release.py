#!/usr/bin/env python3
"""Create a source-complete PS5 LocalSend release archive.

The default archive timestamp is the current time so extracted files do not
appear to have been created in 1970.  Set SOURCE_DATE_EPOCH (or pass --epoch)
when a reproducible timestamp is required.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
import tarfile
import time
from pathlib import Path, PurePosixPath


RELEASE_ROOT = PurePosixPath("ps5localsend")
TOP_LEVEL_FILES = (
    "ps5localsend.elf",
    "config.example.ini",
    "README.md",
    "CHANGELOG.md",
    "LICENSE",
    "LICENSES/THIRD_PARTY.md",
)
SOURCE_FILES = ("Makefile", "config.example.ini", "LICENSE")
SOURCE_TREES = (".github", "docs", "LICENSES", "src", "tests", "tools", "web", "third_party")
RELEASE_FILE_OVERRIDES = {
    "README.md": "docs/github/README.md",
    "CHANGELOG.md": "docs/github/CHANGELOG.md",
}


def file_mode(path: Path) -> int:
    return 0o755 if os.access(path, os.X_OK) else 0o644


def tar_info(name: PurePosixPath, size: int, mode: int, epoch: int) -> tarfile.TarInfo:
    info = tarfile.TarInfo(str(name))
    info.size = size
    info.mode = mode
    info.mtime = epoch
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    return info


def add_bytes(archive: tarfile.TarFile, name: PurePosixPath, data: bytes,
              mode: int, epoch: int) -> None:
    archive.addfile(tar_info(name, len(data), mode, epoch), io.BytesIO(data))


def source_paths(root: Path):
    for relative in SOURCE_FILES:
        yield Path(relative)
    for tree in SOURCE_TREES:
        base = root / tree
        for path in sorted(candidate for candidate in base.rglob("*") if candidate.is_file()):
            if "__pycache__" not in path.parts and path.suffix != ".pyc":
                yield path.relative_to(root)


def package(root: Path, output: Path, epoch: int) -> None:
    elf = root / "ps5localsend.elf"
    digest = hashlib.sha256(elf.read_bytes()).hexdigest()
    checksum = f"{digest}  ps5localsend.elf\n".encode("ascii")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    with temporary.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
                for relative in TOP_LEVEL_FILES:
                    path = root / RELEASE_FILE_OVERRIDES.get(relative, relative)
                    add_bytes(archive, RELEASE_ROOT / relative, path.read_bytes(),
                              file_mode(path), epoch)
                add_bytes(archive, RELEASE_ROOT / "SHA256SUMS", checksum, 0o644, epoch)
                for relative in source_paths(root):
                    path = root / relative
                    archive_name = RELEASE_ROOT / "source" / PurePosixPath(relative.as_posix())
                    add_bytes(archive, archive_name, path.read_bytes(), file_mode(path), epoch)
    temporary.replace(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--epoch", type=int, default=None)
    args = parser.parse_args()
    if args.epoch is None:
        source_date_epoch = os.environ.get("SOURCE_DATE_EPOCH")
        args.epoch = (int(source_date_epoch) if source_date_epoch is not None
                      else int(time.time()))
    root = args.root.resolve()
    if args.epoch < 0:
        parser.error("epoch must be non-negative")
    for relative in TOP_LEVEL_FILES:
        source = RELEASE_FILE_OVERRIDES.get(relative, relative)
        if not (root / source).is_file():
            parser.error(f"required release input is missing: {source}")
    package(root, args.output, args.epoch)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
