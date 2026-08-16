#!/usr/bin/env bash
# license:BSD-3-Clause
#
# Run the driver's dictionary tests outside NVDA, against the real emulator.
#
#   ./run.sh                 # builds a shared libdtalk from ../../build/*.o
#   ROM=path ./run.sh        # a firmware ROM elsewhere
#
# The driver is Windows-only in production (it loads dtalk64.dll), but nothing
# about the dictionary path is: the tests stage a copy of the driver package
# with the host's shared library standing in for the DLL, which is why this
# works on a Mac or a Linux box with no NVDA in sight.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

OBJ=../../build
[[ -d "$OBJ" ]] || { echo "run 'make -C ../..' first" >&2; exit 1; }

# There is no shared-library target in the Makefile - the DLL rules cross
# compile, and libdtalk.a is what the CLI links - so build one here.
case "$(uname -s)" in
    Darwin) SHARED=$OBJ/libdtalk.dylib; FLAGS=-dynamiclib ;;
    *)      SHARED=$OBJ/libdtalk.so;    FLAGS=-shared ;;
esac
if [[ ! -f "$SHARED" ]] || [[ -n "$(find "$OBJ" -name '*.o' -newer "$SHARED" -print -quit)" ]]; then
    ${CXX:-c++} $FLAGS -o "$SHARED" "$OBJ"/*.o
    echo "built $SHARED"
fi

ROM=${ROM:-}
if [[ -z "$ROM" ]]; then
    for candidate in ../../../../doubletalkpc.bin ../synthDrivers/doubletalkpc/doubletalkpc.bin; do
        [[ -f "$candidate" ]] && ROM=$candidate && break
    done
fi
[[ -n "$ROM" && -f "$ROM" ]] || {
    echo "no firmware ROM - set ROM=<path to doubletalkpc.bin>" >&2; exit 1; }

echo "== dictionary file ordering"
python3 test_dictfiles.py ../synthDrivers/doubletalkpc

echo
echo "== the driver, against the emulator"
python3 test_dictionaries.py ../synthDrivers/doubletalkpc "$SHARED" "$ROM"
