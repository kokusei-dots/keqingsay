#!/bin/sh

set -e
cd "$(dirname "$0")/.."
[ -d build ] || meson setup build --prefix /usr
meson compile -C build
sudo meson install -C build
