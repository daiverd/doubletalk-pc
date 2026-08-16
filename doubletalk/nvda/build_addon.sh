#!/usr/bin/env bash
# license:BSD-3-Clause
# copyright-holders:David Sexton
# Package the NVDA add-on. Requires the DLLs already placed in
# synthDrivers/doubletalkpc/ (see README.md).
#
# Default build is PUBLIC-SAFE: the proprietary firmware ROM is NOT
# included (users supply their own doubletalkpc.bin; the driver stays
# unavailable until it exists). Pass --with-rom for a personal build that
# bundles the ROM - that package must never be distributed.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

WITH_ROM=0
[[ "${1:-}" == "--with-rom" ]] && WITH_ROM=1

# Refresh the DLLs from the build tree if they differ from what is here.
# Copying them in by hand is easy to forget, and forgetting it produces the
# worst kind of bug: a package that builds, installs and runs, but ships the
# previous library. That is not hypothetical - it happened during the
# dictionary work, where the symptom was a driver that simply never applied a
# dictionary. If the build tree has no copy, whatever is here already is used.
for pair in "build/win32/dtalk.dll:dtalk.dll" "build/win64/dtalk64.dll:dtalk64.dll"; do
    src="../${pair%%:*}"
    dst="synthDrivers/doubletalkpc/${pair##*:}"
    if [[ -f "$src" ]] && ! cmp -s "$src" "$dst"; then
        cp "$src" "$dst"
        echo "refreshed $dst from $src"
    fi
done

for f in synthDrivers/doubletalkpc/dtalk.dll synthDrivers/doubletalkpc/dtalk64.dll; do
    [[ -f "$f" ]] || { echo "missing $f - run 'make -C .. windows' first, or see README.md" >&2; exit 1; }
done

cp ../NOTICE synthDrivers/doubletalkpc/NOTICE.txt

# globalPlugins/ carries the dictionaries settings category. It has to be a
# global plugin rather than part of the driver - a driver module comes and goes
# with the synthesizer, and a settings category cannot - so it is a second
# directory in the package and easy to leave out of this line.
rm -f doubletalkpc.nvda-addon
if [[ $WITH_ROM == 1 ]]; then
    [[ -f synthDrivers/doubletalkpc/doubletalkpc.bin ]] || { echo "missing ROM for --with-rom build" >&2; exit 1; }
    zip -r -q doubletalkpc.nvda-addon manifest.ini synthDrivers globalPlugins \
        -x "*/__pycache__/*"
    echo "wrote doubletalkpc.nvda-addon (PRIVATE build - bundles the proprietary ROM, do not distribute)"
else
    zip -r -q doubletalkpc.nvda-addon manifest.ini synthDrivers globalPlugins \
        -x "synthDrivers/doubletalkpc/doubletalkpc.bin" -x "*/__pycache__/*"
    echo "wrote doubletalkpc.nvda-addon (public-safe, no ROM - user must supply doubletalkpc.bin)"
fi
unzip -l doubletalkpc.nvda-addon
