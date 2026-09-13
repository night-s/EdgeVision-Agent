#!/usr/bin/env python3
"""Local-file smoke test; run with the PC requirements installed."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import cv2
import numpy as np

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="edge-viewer-test-") as directory:
    directory=Path(directory)
    video=directory/"input.avi"
    writer=cv2.VideoWriter(str(video),cv2.VideoWriter_fourcc(*"MJPG"),20,(160,120))
    assert writer.isOpened()
    for index in range(30):
        frame=np.full((120,160,3),index*7,dtype=np.uint8)
        writer.write(frame)
    writer.release()
    output=directory/"report"
    subprocess.run([sys.executable,str(root/"tools/pc_viewer.py"),"--source",str(video),
                    "--headless","--seconds","10","--output",str(output)],check=True,timeout=20)
    report=json.loads((output/"report.json").read_text())
    assert report["frames"]==30
    assert report["workers_stopped"]
    assert report["control_errors"]==0
    assert report["read_errors"]==0
    assert report["passed"]
    print("Local-file decoding and worker shutdown passed")
