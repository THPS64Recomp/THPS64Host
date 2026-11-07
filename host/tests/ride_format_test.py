#!/usr/bin/env python3
"""ROM-free ride parser checks against a built host. Valid files reach the empty-ROM check (exit 3).

Usage: python3 -I ride_format_test.py BINARY --family thps1 [--out LOCAL_EVIDENCE_DIR]
The files contain controller inputs only. No game is started and no ROM is loaded.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def cases(family):
    header = f"# thps ride v2 {family} reads=1\n"
    return {
        "empty": ("", 2),
        "malformed": (header + "not input\n", 2),
        "nan": (header + "0000 nan 0 1\n", 2),
        "infinity": (header + "0000 0 inf 1\n", 2),
        "range": (header + "0000 1.1 0 1\n", 2),
        "buttons": (header + "10000 0 0 1\n", 2),
        "presence": (header + "0000 0 0 2\n", 2),
        "truncated": (f"# thps ride v2 {family} reads=2\n0000 0 0 1\n", 2),
        "wrong-family": ("# thps ride v2 wrong reads=1\n0000 0 0 1\n", 2),
        "unknown-version": (f"# thps ride v3 {family} reads=1\n0000 0 0\n", 2),
        "trailing-junk": (header + "0000 0 0 1 bad\n", 2),
        "double-header": (header + header + "0000 0 0 1\n", 2),
        "legacy-valid": ("# thps ride v1 reads=1\n0000 0.0000 0.0000\n", 3),
        "v2-valid": (header + "8000 0.999964118 -0.411764711 1\n", 3),
        "v2-absent": (header + "0000 0 0 0\n", 3),
        "v2-course-boundary": (header.rstrip() + " start=course\n0000 0 0 1\n", 3),
        "v2-run-boundary": (header.rstrip() + " start=run\n0000 0 0 1\n", 2 if family == "thps1" else 3),
        "v2-unknown-boundary": (header.rstrip() + " start=other\n0000 0 0 1\n", 2),
    }


def check(binary, family, out):
    out.mkdir(parents=True, exist_ok=True)
    empty = out / "empty.rom"
    empty.write_bytes(b"")
    results = []
    for name, (data, wanted) in cases(family).items():
        path = out / f"{name}.ride"
        path.write_text(data, encoding="ascii")
        try:
            proc = subprocess.run([str(binary), "--ride-play", str(path), "--check-rom", str(empty)],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=15)
            (out / f"{name}.log").write_text(proc.stdout, encoding="utf-8")
            result = dict(case=name, expected=wanted, exit=proc.returncode, passed=proc.returncode == wanted)
        except subprocess.TimeoutExpired:
            result = dict(case=name, expected=wanted, exit=None, passed=False, error="host did not exit within 15 seconds")
        results.append(result)
        if not result["passed"]:
            print(f"FAIL {family} {name}: expected exit {wanted}, got {result['exit']}; see {out / (name + '.log')}")
    (out / "results.json").write_text(json.dumps(dict(family=family, results=results), indent=2) + "\n", encoding="utf-8")
    passed = sum(row["passed"] for row in results)
    print(f"ride format: {family} {passed}/{len(results)} pass")
    return 0 if passed == len(results) else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--family", required=True, choices=("thps1", "thps2", "thps3"))
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if args.out:
        return check(binary, args.family, args.out.resolve())
    with tempfile.TemporaryDirectory(prefix="thps-ride-format-") as directory:
        return check(binary, args.family, Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())
