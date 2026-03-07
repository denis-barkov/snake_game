#!/usr/bin/env python3
import argparse
import json
import re
import sys
from pathlib import Path
from typing import List, Dict


SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
ENTRY_RE = re.compile(r"^##\s+(\d+\.\d+\.\d+)\s+-\s+(\d{4}-\d{2}-\d{2})\s*$")


def parse_changelog(path: Path) -> List[Dict]:
    if not path.exists():
        raise RuntimeError(f"Missing changelog: {path}")

    lines = path.read_text(encoding="utf-8").splitlines()
    entries: List[Dict] = []
    current = None

    for raw in lines:
        line = raw.strip()
        if not line:
            continue
        m = ENTRY_RE.match(line)
        if m:
            if current is not None:
                entries.append(current)
            current = {"version": m.group(1), "release_date": m.group(2), "notes": []}
            continue
        if line.startswith("- "):
            if current is None:
                raise RuntimeError("Bullet point found before any version header")
            current["notes"].append(line[2:].strip())
            continue

    if current is not None:
        entries.append(current)

    if not entries:
        raise RuntimeError("No changelog entries found")

    seen = set()
    for idx, e in enumerate(entries):
        version = e["version"]
        if not SEMVER_RE.match(version):
            raise RuntimeError(f"Invalid SemVer version: {version}")
        if version in seen:
            raise RuntimeError(f"Duplicate version found: {version}")
        seen.add(version)
        notes = e["notes"]
        if len(notes) < 3 or len(notes) > 7:
            raise RuntimeError(
                f"Version {version} must have 3-7 bullet points, found {len(notes)}"
            )
        if idx == 0 and not notes:
            raise RuntimeError("Top entry has no notes")

    return entries


def generate(entries: List[Dict]) -> Dict:
    return {
        "current_version": entries[0]["version"],
        "entries": entries,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate world evolution log JSON from CHANGELOG.md"
    )
    parser.add_argument("--input", default="CHANGELOG.md")
    parser.add_argument("--output", default="assets/world_evolution_log.json")
    parser.add_argument("--validate-only", action="store_true")
    args = parser.parse_args()

    try:
        entries = parse_changelog(Path(args.input))
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    if args.validate_only:
        print(f"OK: parsed {len(entries)} changelog entries")
        return 0

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    payload = generate(entries)
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out_path} ({len(entries)} entries, current={payload['current_version']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
