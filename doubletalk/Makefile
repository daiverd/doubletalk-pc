# Standalone DoubleTalk PC emulator (vendored MAME 80C188EB core + shim)
CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Ishim -Imame -Wall -Wno-unused-variable

OBJS = build/emu.o build/i86.o build/i186.o build/doubletalk_board.o

all: build/dtalk_cli

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

build/dtalk_cli: dtalk_cli.cpp doubletalk_board.h $(OBJS) | build
	$(CXX) $(CXXFLAGS) $< $(OBJS) -o $@

clean:
	rm -rf build

.PHONY: all clean
