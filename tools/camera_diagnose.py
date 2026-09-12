#!/usr/bin/env python3
"""Read camera capabilities; optionally measure auto/manual exposure and restore controls."""
import argparse
import datetime
import json
from pathlib import Path
import subprocess
import time

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--device", default="/dev/video10")
    ap.add_argument("--exposure-test", action="store_true")
    ap.add_argument("--output", default="output/camera-diagnosis.json")
    args = ap.parse_args()
    def run(command, timeout=45):
        start = time.monotonic()
        p = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        return dict(command=command, returncode=p.returncode, stdout=p.stdout, stderr=p.stderr,
                    elapsed_s=time.monotonic()-start)
    def ctl(*options): return run(["v4l2-ctl", "-d", args.device, *options])
    report = dict(date=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  source_commit=subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip(),
                  device=args.device, environment=run(["uname","-a"]),
                  capabilities=ctl("--all"), formats=ctl("--list-formats-ext"), usb=run(["lsusb","-t"]))
    if args.exposure_test:
        if subprocess.run(["pgrep","-x","edge_agent"],capture_output=True).returncode == 0:
            raise SystemExit("Stop edge_agent before exclusive capture tests")
        old = ctl("--get-ctrl=exposure_auto,exposure_absolute")
        values = {line.split(":")[0].strip():int(line.split(":")[1])
                  for line in old["stdout"].splitlines() if ":" in line}
        if not all(k in values for k in ("exposure_auto","exposure_absolute")):
            raise SystemExit("Cannot read original exposure controls")
        report["original_controls"] = values
        report["runs"] = []
        try:
            for name, absolute in (("auto", None),("manual_10ms",100),("manual_30ms",300)):
                changes = [ctl("--set-ctrl=exposure_auto=" + ("3" if absolute is None else "1"))]
                if absolute is not None: changes.append(ctl("--set-ctrl=exposure_absolute="+str(absolute)))
                if any(r["returncode"] for r in changes): raise RuntimeError(changes)
                result = ctl("--set-fmt-video=width=640,height=480,pixelformat=YUYV",
                             "--set-parm=30","--stream-mmap=4","--stream-poll",
                             "--stream-count=180","--stream-to=/dev/null")
                report["runs"].append(dict(name=name,changes=changes,result=result,after=ctl("--get-parm")))
        finally:
            restore = [ctl("--set-ctrl=exposure_auto=1"),
                       ctl("--set-ctrl=exposure_absolute="+str(values["exposure_absolute"])),
                       ctl("--set-ctrl=exposure_auto="+str(values["exposure_auto"]))]
            report["restore"] = restore
            report["restored_ok"] = all(r["returncode"] == 0 for r in restore)
            path=Path(args.output);path.parent.mkdir(parents=True,exist_ok=True)
            path.write_text(json.dumps(report,indent=2),encoding="utf-8")
    else:
        path=Path(args.output);path.parent.mkdir(parents=True,exist_ok=True)
        path.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(args.output)

if __name__ == "__main__": main()
