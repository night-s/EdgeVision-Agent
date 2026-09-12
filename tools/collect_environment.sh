#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
uname -a
cat /proc/device-tree/model 2>/dev/null
printf '\n'
free -m
cmake --version | head -1
g++ --version | head -1
pkg-config --modversion opencv4 librga rockchip_mpp gstreamer-1.0
sha256sum models/yolov5s-640-640.rknn lib/librknnrt.so
v4l2-ctl -d "${1:-/dev/video10}" --all
v4l2-ctl -d "${1:-/dev/video10}" --list-formats-ext
gst-inspect-1.0 mpph264enc
