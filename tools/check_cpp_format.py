#!/usr/bin/env python3
"""Check project-owned C++ formatting with clang-format 18.1.8."""
import argparse
from pathlib import Path
import subprocess
import sys
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument("--formatter",default="clang-format")
args=ap.parse_args()
root=Path(__file__).resolve().parents[1]
version=subprocess.check_output([args.formatter,"--version"],text=True)
if "18.1.8" not in version:
    raise SystemExit("Expected clang-format 18.1.8, got "+version.strip())
files=[root/"main.cpp"]
for folder in ("app","core","hardware","media","tests"):
    files.extend(p for p in (root/folder).rglob("*") if p.suffix in (".h",".cpp"))
bad=[]
for path in files:
    formatted=subprocess.check_output([args.formatter,"--style=file",str(path)])
    if formatted.replace(b"\r\n",b"\n") != path.read_bytes().replace(b"\r\n",b"\n"):
        bad.append(str(path.relative_to(root)))
if bad:
    print("Formatting differs:\n"+"\n".join(bad))
    sys.exit(1)
print("Formatting passed for",len(files),"files")
