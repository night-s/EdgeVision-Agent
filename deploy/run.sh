#!/usr/bin/env bash
set -euo pipefail
prefix="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$prefix/bin/edge_agent" "${1:-/etc/edgevision/config.json}"
