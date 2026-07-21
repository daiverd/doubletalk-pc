#!/usr/bin/env bash
# Package the NVDA add-on. Requires dtalk.dll and doubletalkpc.bin already
# placed in synthDrivers/doubletalkpc/ (see README.md).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

for f in synthDrivers/doubletalkpc/dtalk.dll synthDrivers/doubletalkpc/dtalk64.dll synthDrivers/doubletalkpc/doubletalkpc.bin; do
    [[ -f "$f" ]] || { echo "missing $f - see README.md" >&2; exit 1; }
done

rm -f doubletalkpc.nvda-addon
zip -r -q doubletalkpc.nvda-addon manifest.ini synthDrivers
echo "wrote doubletalkpc.nvda-addon"
