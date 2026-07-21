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

# 32-bit Windows DLL for the NVDA synth driver (needs g++-mingw-w64-i686).
# NVDA is a 32-bit process, hence i686.
MINGW_CXX ?= i686-w64-mingw32-g++
MINGW_FLAGS = -std=c++20 -O2 -Ishim -Imame -Wall -DDTALK_DLL -DDTALK_BUILD

WIN_OBJS = build/win32/emu.o build/win32/i86.o build/win32/i186.o \
	build/win32/doubletalk_board.o build/win32/dtalk.o

win32: build/win32/dtalk.dll

build/win32:
	mkdir -p build/win32

build/win32/emu.o: shim/emu.cpp shim/emu.h | build/win32
	$(MINGW_CXX) $(MINGW_FLAGS) -c $< -o $@
build/win32/i86.o: mame/i86.cpp shim/emu.h | build/win32
	$(MINGW_CXX) $(MINGW_FLAGS) -c $< -o $@
build/win32/i186.o: mame/i186.cpp shim/emu.h | build/win32
	$(MINGW_CXX) $(MINGW_FLAGS) -c $< -o $@
build/win32/doubletalk_board.o: doubletalk_board.cpp doubletalk_board.h shim/emu.h | build/win32
	$(MINGW_CXX) $(MINGW_FLAGS) -c $< -o $@
build/win32/dtalk.o: dtalk.cpp dtalk.h doubletalk_board.h shim/emu.h | build/win32
	$(MINGW_CXX) $(MINGW_FLAGS) -c $< -o $@

build/win32/dtalk.dll: $(WIN_OBJS)
	$(MINGW_CXX) -shared -static-libgcc -static-libstdc++ -o $@ $(WIN_OBJS) \
		-Wl,--out-implib,build/win32/dtalk.lib

.PHONY: all clean win32
