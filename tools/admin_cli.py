#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path


def main() -> int:
    snakecli = Path(__file__).with_name("snakecli.py")
    if not snakecli.exists():
        print("ERROR: snakecli.py not found", file=sys.stderr)
        return 1
    return subprocess.call([sys.executable, str(snakecli), *sys.argv[1:]], env=os.environ.copy())


if __name__ == "__main__":
    raise SystemExit(main())
