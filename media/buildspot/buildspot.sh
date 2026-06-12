#!/bin/sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

exec python3 "$SCRIPT_DIR/buildspot-console.py" sync "$@"
