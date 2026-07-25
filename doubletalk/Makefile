# license:BSD-3-Clause
# copyright-holders:David Sexton
# Standalone DoubleTalk PC emulator (vendored MAME 80C188EB core + shim)
CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Ishim -Imame -Wall -Wno-unused-variable

OBJS = build/emu.o build/i86.o build/i186.o build/doubletalk_board.o build/dtalk.o

all: build/dtalk_cli build/libdtalk.a

build:
	mkdir -p build

build/emu.o: shim/emu.cpp shim/emu.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/i86.o: mame/i86.cpp shim/emu.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/i186.o: mame/i186.cpp shim/emu.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/doubletalk_board.o: doubletalk_board.cpp doubletalk_board.h shim/emu.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/dtalk.o: dtalk.cpp dtalk.h doubletalk_board.h shim/emu.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/libdtalk.a: $(OBJS) | build
	ar rcs $@ $(OBJS)

build/dtalk_cli: dtalk_cli.cpp doubletalk_board.h dtalk.h $(OBJS) | build
	$(CXX) $(CXXFLAGS) $< $(OBJS) -o $@

clean:
	rm -rf build

# Windows DLLs for the NVDA synth driver. NVDA 2025.2+ is a 64-bit
# process (dtalk64.dll); older NVDA is 32-bit (dtalk.dll) - the driver
# loads whichever matches its process at runtime. Needs
# g++-mingw-w64-x86-64 and/or g++-mingw-w64-i686.
#
# The DLL must not depend on any mingw runtime: NVDA users have no mingw
# installation, and a missing libwinpthread-1.dll shows up as the driver
# silently failing to load. -static-libgcc/-static-libstdc++ alone are NOT
# enough - Debian ships mingw-w64 in two thread models, selectable with
# update-alternatives, and under the *posix* model libstdc++ pulls in
# libwinpthread-1.dll, which neither of those flags covers. Plain -static
# does cover it. Under the *win32* model it is a no-op (byte-for-byte the
# same DLL, modulo the PE timestamp), so it is safe to pass unconditionally
# rather than probing which alternative the builder happens to have selected.
# This project's own code uses no threads; the dependency comes entirely
# from libstdc++'s posix build.
MINGW_FLAGS = -std=c++20 -O2 -Ishim -Imame -Wall -DDTALK_DLL -DDTALK_BUILD \
	-static -static-libgcc -static-libstdc++
WIN_SRCS = shim/emu.cpp mame/i86.cpp mame/i186.cpp doubletalk_board.cpp dtalk.cpp
WIN_DEPS = shim/emu.h dtalk.h doubletalk_board.h $(WIN_SRCS)

win32: build/win32/dtalk.dll
win64: build/win64/dtalk64.dll
windows: win32 win64

build/win32/dtalk.dll: $(WIN_DEPS)
	mkdir -p build/win32
	i686-w64-mingw32-g++ $(MINGW_FLAGS) -shared \
		-o $@ $(WIN_SRCS) -Wl,--out-implib,build/win32/dtalk.lib

build/win64/dtalk64.dll: $(WIN_DEPS)
	mkdir -p build/win64
	x86_64-w64-mingw32-g++ $(MINGW_FLAGS) -shared \
		-o $@ $(WIN_SRCS) -Wl,--out-implib,build/win64/dtalk64.lib

.PHONY: all clean win32 win64 windows
