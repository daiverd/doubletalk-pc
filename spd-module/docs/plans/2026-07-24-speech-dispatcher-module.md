# DoubleTalk Speech Dispatcher Module Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:test-driven-development to implement this plan task-by-task.

**Goal:** Build a native Speech Dispatcher output module under `spd-module/` that links `../doubletalk/build/libdtalk.a`, loads the firmware path from configuration, streams 16-bit PCM, maps settings and voices, handles index marks, and stops promptly.

**Architecture:** Keep protocol-independent configuration, setting mapping, and text/SSML conversion in `module_core.cpp`, covered by a small C++ test executable. Keep Speech Dispatcher callbacks and DoubleTalk lifetime/audio streaming in `sd_doubletalk.cpp`. Link the final executable with the installed BSD-licensed `libspeechd_module` module basis and the existing static DoubleTalk archive.

**Tech Stack:** C++20, GNU Make, Speech Dispatcher 0.12 module API, `libspeechd_module`, pthreads, DoubleTalk C API.

---

### Task 1: Configuration parser

**Files:**
- Create: `spd-module/src/module_core.h`
- Create: `spd-module/src/module_core.cpp`
- Create: `spd-module/tests/test_module_core.cpp`
- Create: `spd-module/Makefile`

**Step 1: Write the failing test**

Add tests asserting that:

```cpp
auto cfg = parse_config_text(
    "DoubleTalkRomPath \"/tmp/double talk.bin\"\n"
    "DoubleTalkLowPassHz 4800\n");
assert(cfg.rom_path == "/tmp/double talk.bin");
assert(cfg.lowpass_hz == 4800);
```

Also assert that missing ROM paths, unknown directives, trailing arguments,
invalid integers, and out-of-range low-pass values return useful errors.

**Step 2: Run test to verify it fails**

Run: `make -C spd-module test`

Expected: FAIL because `module_core.h` and the parser do not exist.

**Step 3: Write minimal implementation**

Implement:

```cpp
struct ModuleConfig {
    std::string rom_path;
    uint32_t lowpass_hz = 3000;
};

ParseResult parse_config_text(std::string_view text);
ParseResult parse_config_file(const std::string &path);
```

Support blank lines, `#` comments, quoted paths with spaces, and the two
approved directives. Reject malformed or unknown input.

**Step 4: Run test to verify it passes**

Run: `make -C spd-module test`

Expected: PASS.

### Task 2: Settings and voice mapping

**Files:**
- Modify: `spd-module/src/module_core.h`
- Modify: `spd-module/src/module_core.cpp`
- Modify: `spd-module/tests/test_module_core.cpp`

**Step 1: Write the failing test**

Test endpoint and midpoint mappings:

```cpp
assert(map_rate(-100) == 0);
assert(map_rate(0) == 5);
assert(map_rate(100) == 9);
assert(map_pitch(-100) == 0);
assert(map_pitch(0) == 50);
assert(map_pitch(100) == 99);
assert(map_volume(0) == 5);
```

Test all eight voice names and verify a prefix is ordered as voice, rate,
pitch, volume.

**Step 2: Run test to verify it fails**

Run: `make -C spd-module test`

Expected: FAIL because mapping functions are absent.

**Step 3: Write minimal implementation**

Add a `SpeechSettings` value object, clamped range mapping helpers, voice-name
lookup, and `build_command_prefix()`.

**Step 4: Run test to verify it passes**

Run: `make -C spd-module test`

Expected: PASS.

### Task 3: Text, SSML, and index-mark conversion

**Files:**
- Modify: `spd-module/src/module_core.h`
- Modify: `spd-module/src/module_core.cpp`
- Modify: `spd-module/tests/test_module_core.cpp`

**Step 1: Write the failing test**

Test that conversion:

- preserves printable ASCII;
- replaces non-ASCII/control input with spaces;
- strips ordinary XML tags;
- decodes `amp`, `lt`, `gt`, `quot`, and `apos`;
- converts `<mark name="checkpoint"/>` to `Ctrl-A 0I`;
- records `{0, "checkpoint"}`;
- rejects malformed marks and more than 100 marks.

**Step 2: Run test to verify it fails**

Run: `make -C spd-module test`

Expected: FAIL because conversion is absent.

**Step 3: Write minimal implementation**

Implement a bounded single-pass converter returning firmware bytes, marker
mappings, and an error string. Do not implement general XML; recognize tags,
quoted mark names, and the five standard entities only.

**Step 4: Run test to verify it passes**

Run: `make -C spd-module test`

Expected: PASS.

### Task 4: Speech Dispatcher adapter and DoubleTalk streaming

**Files:**
- Create: `spd-module/src/sd_doubletalk.cpp`
- Modify: `spd-module/Makefile`

**Step 1: Write the failing build check**

Add the final target and run:

`make -C spd-module build/sd_doubletalk`

Expected: FAIL because the module callbacks are not implemented.

**Step 2: Implement the minimal adapter**

Implement all callbacks required by `spd_module_main.h`:

- `module_config`
- `module_init`
- `module_list_voices`
- `module_set`
- `module_audio_set`
- `module_audio_init`
- `module_loglevel_set`
- `module_debug`
- `module_loop`
- `module_speak_sync`
- `module_pause`
- `module_stop`
- `module_close`

`module_init()` reads and validates the 512 KiB ROM, creates the emulator, sets
the configured low-pass, and selects server audio. `module_speak_sync()` queues
the command prefix and converted request, sends bounded PCM chunks through
`module_tts_output_server()`, reports reached marks, and emits begin/end/stop
events correctly.

**Step 3: Run the narrow build**

Run: `make -C spd-module build/sd_doubletalk`

Expected: PASS with no warnings.

### Task 5: Configuration and usage documentation

**Files:**
- Create: `spd-module/doubletalk.conf`
- Create: `spd-module/README.md`

**Step 1: Add example configuration**

Document the required absolute ROM path and optional low-pass value without
including or copying the proprietary ROM.

**Step 2: Document build and registration**

Include:

```text
AddModule "doubletalk" "/absolute/path/to/spd-module/build/sd_doubletalk" \
          "/absolute/path/to/spd-module/doubletalk.conf"
```

Document prerequisites, `make`, `make test`, user/system configuration, all
voice names, supported controls, and `spd-say -o doubletalk "Hello"` smoke
testing.

**Step 3: Check documentation paths**

Run: `rg -n "DoubleTalkRomPath|AddModule|spd-say" spd-module`

Expected: all setup steps are present.

### Task 6: Verification

**Files:**
- Verify only; do not modify `doubletalk/`

**Step 1: Run the unit tests**

Run: `make -C spd-module test`

Expected: PASS.

**Step 2: Run a clean rebuild**

Run:

`timeout 10m bash -lc 'make -C spd-module clean all > /tmp/codex-doubletalk-spd-build.log 2>&1'; rc=$?; echo "exit=$rc"; tail -n 120 /tmp/codex-doubletalk-spd-build.log; exit $rc`

Expected: exit 0 with no compiler warnings.

**Step 3: Verify workspace isolation**

Run:

`git status --short && git diff --check && find spd-module -maxdepth 4 -type f | sort`

Expected: all new files are under `spd-module/`, with no changes beneath
`doubletalk/`.

**Step 4: Optional ROM integration smoke test**

If the configured ROM exists, drive `INIT`, `SET`, and `SPEAK` requests through
the module and confirm `299`, `200`, `701`, `705`, and `702` responses. If no
ROM is available, report the integration test as skipped rather than failing
the build.

