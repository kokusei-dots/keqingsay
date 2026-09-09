#!/bin/sh

set -e
cd "$(dirname "$0")/.."
[ -d build ] || meson setup build
meson compile -C build
echo "built: build/keqingsay"
