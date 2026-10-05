"""Write a C6 link artifact binding deciding sources and invoked bytes."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path


def digest(path: Path, text: bool = False) -> str:
    data = path.read_bytes()
    if text:
        data = data.replace(b"\r\n", b"\n")
    return hashlib.sha256(data).hexdigest()


def manifest(repo: Path, executable: Path, library: Path, sources: list[Path],
             invoked: list[Path]) -> dict:
    repo = repo.resolve()
    source_hashes = {}
    for source in sources:
        path = source.resolve()
        source_hashes[path.relative_to(repo).as_posix()] = digest(path, text=True)
    byte_hashes = {}
    for path in [executable, library, *invoked]:
        resolved = path.resolve()
        byte_hashes[resolved.name] = digest(resolved)
    return {"schema": "pkv-c6-source-set/1", "sources": dict(sorted(source_hashes.items())),
            "invoked": dict(sorted(byte_hashes.items()))}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--source", action="append", default=[], type=Path)
    parser.add_argument("--invoked", action="append", default=[], type=Path)
    args = parser.parse_args()
    record = manifest(args.repo, args.exe, args.library, args.source, args.invoked)
    output = args.exe.with_suffix(args.exe.suffix + ".source-set.json")
    temporary = output.with_name(output.name + ".partial")
    try:
        temporary.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
