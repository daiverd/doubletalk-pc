CXX ?= g++
CPPFLAGS += -Isrc -I../doubletalk
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror

SPEECHD_CFLAGS := $(shell pkg-config --cflags speech-dispatcher 2>/dev/null)
SPEEXDSP_CFLAGS := $(shell pkg-config --cflags speexdsp 2>/dev/null)
SPEEXDSP_LIBS := $(shell pkg-config --libs speexdsp 2>/dev/null)

.PHONY: all test clean

all: build/sd_doubletalk

test: build/test_module_core
	./build/test_module_core

build:
	mkdir -p $@

build/module_core.o: src/module_core.cpp src/module_core.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/pcm_resampler.o: src/pcm_resampler.cpp src/pcm_resampler.h | build
	$(CXX) $(CPPFLAGS) $(SPEEXDSP_CFLAGS) $(CXXFLAGS) -c $< -o $@

build/test_module_core: tests/test_module_core.cpp build/module_core.o build/pcm_resampler.o | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(SPEEXDSP_LIBS) -o $@

build/sd_doubletalk.o: src/sd_doubletalk.cpp src/module_core.h src/pcm_resampler.h src/spd_audio.h ../doubletalk/dtalk.h | build
	$(CXX) $(CPPFLAGS) $(SPEECHD_CFLAGS) $(CXXFLAGS) -c $< -o $@

build/sd_doubletalk: build/sd_doubletalk.o build/module_core.o build/pcm_resampler.o ../doubletalk/build/libdtalk.a
	$(CXX) $(CXXFLAGS) $^ -lspeechd_module $(SPEEXDSP_LIBS) -pthread -o $@

clean:
	rm -rf build
