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

for f in synthDrivers/doubletalkpc/dtalk.dll synthDrivers/doubletalkpc/dtalk64.dll; do
    [[ -f "$f" ]] || { echo "missing $f - see README.md" >&2; exit 1; }
done

cp ../NOTICE synthDrivers/doubletalkpc/NOTICE.txt

rm -f doubletalkpc.nvda-addon
if [[ $WITH_ROM == 1 ]]; then
    [[ -f synthDrivers/doubletalkpc/doubletalkpc.bin ]] || { echo "missing ROM for --with-rom build" >&2; exit 1; }
    zip -r -q doubletalkpc.nvda-addon manifest.ini synthDrivers
    echo "wrote doubletalkpc.nvda-addon (PRIVATE build - bundles the proprietary ROM, do not distribute)"
else
    zip -r -q doubletalkpc.nvda-addon manifest.ini synthDrivers -x "synthDrivers/doubletalkpc/doubletalkpc.bin"
    echo "wrote doubletalkpc.nvda-addon (public-safe, no ROM - user must supply doubletalkpc.bin)"
fi
