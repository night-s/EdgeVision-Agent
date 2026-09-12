#!/usr/bin/env bash
# Isolated fallback for vendor images where runtime packages are newer than apt dev packages.
# Normal distributions should install matching GStreamer app and RTSP server development packages.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
mkdir -p .deps/debs
cd .deps/debs
apt-get download libgstreamer-plugins-base1.0-dev libgstrtspserver-1.0-dev libgstrtspserver-1.0-0
for package in ./*.deb; do dpkg-deb -x "$package" ..; done
echo "Dependencies extracted under .deps; system runtime packages were not replaced."
