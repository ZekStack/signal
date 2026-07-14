#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read_properties(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip()
    return values


def fail(message: str) -> None:
    raise SystemExit(message)


def main() -> None:
    library_json = json.loads((ROOT / "library.json").read_text(encoding="utf-8"))
    properties = read_properties(ROOT / "library.properties")
    json_version = str(library_json.get("version", ""))
    properties_version = properties.get("version", "")

    if not re.fullmatch(r"\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?", json_version):
        fail(f"library.json has invalid version: {json_version!r}")
    if json_version != properties_version:
        fail(
            "version mismatch: "
            f"library.json={json_version!r}, library.properties={properties_version!r}"
        )

    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    if "| Status | `0.1.0` release candidate |" not in readme:
        fail("README status does not identify the 0.1.0 release candidate")

    if len(sys.argv) > 2:
        fail("usage: check_version.py [vX.Y.Z]")
    if len(sys.argv) == 2:
        tag = sys.argv[1]
        if not tag.startswith("v"):
            fail(f"release tag must start with v: {tag!r}")
        tag_version = tag[1:]
        if tag_version != json_version:
            fail(f"tag/version mismatch: tag={tag_version!r}, metadata={json_version!r}")

    print(f"Signal version metadata is consistent: {json_version}")


if __name__ == "__main__":
    main()
