# Standalone DoubleTalk Port — Status, Plan, and Handoff

Written as a full context dump before clearing the session that did the
rusty_tts MAME-based provider work. If you're picking this up cold, this
file plus the three it points to (`native/retrochip/doubletalk_notes/
PORTING.md` in `rusty_tts`, `notes/standalone_port_prep.md` and
`notes/phase1_findings.md` in this repo) should be everything you need.

## Where things stand

**Done, working, committed:** a MAME-based DoubleTalk PC voice for
`rusty_tts` (`providers/doubletalk.py`), following the same pattern as
that repo's other real-hardware-automation providers (Textalker, Votrax
Type 'N Talk/Personal Speech System). Boots a real emulated PC (pcv20 +
GLaBIOS) with the vendored DoubleTalk ISA card driver attached, drives it
over its real host protocol, captures genuine firmware-generated speech.
Verified end-to-end through the live deployed API, latency-tuned, volume-
matched against the other providers. All on `rusty_tts`'s `main` branch,
9 commits ahead of `origin/main` (not yet pushed to origin - just sitting
local/committed). Chronologically, the commits are:
`0e27e04` (vendor MAME driver) → `c0d7eff` (provider) → `6257d87` (volume)
→ `33d8227` (latency) → `39430d5` (+10% overclock, **superseded**) →
`ff18a84` (2x overclock + lossless sample-rate compensation, current) →
`e380aa1` (Docker build-cache fix) → `ac8e54d` (click comment fix).

**Not started as working code:** the standalone (non-MAME) port. This is
the "do 1" half of an earlier explicit instruction ("do #2 if it's fast,
then don't be lazy, and do 1") - #2 (MAME-based provider) is done; #1
(standalone) is what this file is prepping for.

## Why go standalone at all

MAME works but carries real overhead per request: even after the latency
work above (drain-tail timing fix, build-cache fix, clock doubling), each
request still boots and tears down a full emulated PC + ISA card + MAME
framework. Profiling during that work found the *host PC* emulation alone
(GLaBIOS boot + idle V20 CPU, contributing nothing useful after boot)
costs ~25-30% of total interpreter cycles on top of the card's own
necessary work. A standalone port has no host PC to emulate at all - just
the DoubleTalk card's own 80C188EB core plus a trivial byte-in/audio-out
interface - and matches the existing `native/retrochip/` pattern already
used for votrax.cpp/sp0256.cpp/tms5220.cpp/tms5110.cpp/s14001a.cpp (hand-
ported, dependency-free chip emulators driven by a shared CLI).

## The plan (from `PORTING.md`, still current)

`rusty_tts`'s `native/retrochip/doubletalk_notes/PORTING.md` (245 lines,
BSD-3-Clause material extracted from MAME's `i86.cpp`/`i86.h`/`i186.cpp`/
`i186.h`) is the primary technical reference for the CPU port itself. Key
points from it, condensed:

- **Scope warning**: this is not a quick add-on like the other retrochip
  chips. The CPU core alone is ~4800 lines (i86.cpp ~2600 + i186.cpp
  ~2200) and x86 interpreters have a huge surface area (addressing modes,
  flags, segment overrides, string ops) where subtle bugs produce
  plausible-looking-but-wrong behavior instead of a crash - this exact
  failure mode cost the MAME driver work many iterations even reusing
  MAME's own mature core. Budget it as a real multi-session project.
  Validate *continuously* against the known-good MAME reference, not just
  at the end.
- **Memory map** (confirmed, not guesses): ROM 512KB at physical
  `0x80000-0xFFFFF`, reset vector `0xFFFF0`, RAM `0x00000-0xA0FF` +
  `0xA101-0x1FFFF` (real hardware is 8KB per the manual spec - see below;
  the wider range in the current MAME driver is an untrimmed placeholder),
  host mailbox at `0xA100` (word-aligned).
- **I/O map**: port `0x00` write = DAC byte (audio capture point, driven
  by firmware's internal timer ISR once the 80C188EB timer peripheral is
  correctly modeled - no separate "audio ready" signal needed), port
  `0x40` write = TTS status byte (SYNC/SYNC2/RDY/AF/AE bits, firmware-
  owned), port `0x80` seen in boot trace but unused/unmapped safely.
- **Host protocol**: TTS-port write → latch + INT1 assert (stays
  asserted); firmware ISR at `0x81D26` reads mailbox at `0xA100` as its
  first instruction - **that's** the point to deassert INT1, not a timed
  one-shot (a timed deassert silently dropped interrupts under a same-
  priority race in this project's actual development history - don't
  reintroduce that workaround). Host read of the TTS port just returns
  the latched status byte the firmware itself wrote via port `0x40`.
- **Peripheral Control Block** (the hard part): relocatable via I/O port
  `0xFFA8` (RELREG, EB default `0x00ff`). This firmware doesn't use iRMX
  mode (bit 14 clear) or memory-space PCB (bit 12 clear) - skip
  implementing those branches, dead code for our purposes. Full EB
  byte-offset→register table, interrupt-dispatch priority-scan logic, and
  EOI (specific + non-specific) semantics are all written out in
  `PORTING.md` - don't re-derive, just port directly.
- **Timers**: 3 timers, audio-critical (this is where port `0x00`'s PCM
  stream originates). MAME uses an event-scheduler; the standalone port
  should instead track a plain integer "cycles remaining" countdown per
  timer, decremented by however many cycles the interpreter's step loop
  consumes - simpler, sufficient, no need for MAME's general-purpose
  scheduler abstraction. Confirmed real-world PCM cadence ~10.5kHz at
  10MHz clock → expect ~952 cycles between timer interrupts (verify
  against a real captured trace, don't assume).
- **Suggested build order** (from `PORTING.md`):
  1. Minimal headless 8086/80186 interpreter first - validate it executes
     the ROM's boot sequence up to the first `HLT` at physical `0x80101`
     and matches the exact instruction trace already captured (see
     `full_disasm.txt` and `notes/phase1_findings.md` in this repo).
     Cheap, concrete, early checkpoint before touching interrupts/timers.
  2. Add PCB/interrupt/timer subsystem, get INT1 (host doorbell) working -
     validate against sending one short phrase and confirming the same
     buffer-pointer-advancement behavior already documented.
  3. Add timer-driven INT0 + port-0x00 capture, get real audio out,
     validate against the long-phrase regression test's pass criteria
     (`mame-doubletalk`'s `scripts/run_doubletalk_regression.py` - same
     phrase, same checks: no crash, real sustained audio).
  4. Only then wire into `retrochip`'s CLI and write the Python provider.

## What this session adds on top of `PORTING.md`

`PORTING.md` predates the rusty_tts MAME-provider work in this session -
these are new findings/decisions that should inform the port but aren't
in that file yet (see `notes/standalone_port_prep.md` for the full
writeup):

1. **Architecture decision: free-run the interpreter, don't pace to real
   10MHz timing.** Verified directly: the DAC is written straight from
   CPU-timer-driven code, no separate audio clock domain - so CPU clock
   speed and speech speed/pitch are coupled (tested +10% and 2x
   overclocks on the MAME driver, both shifted speed and pitch together
   by exactly the expected factor). MAME's `-nothrottle` decouples
   emulated time from wall-clock time entirely and gets speed for free
   with correct output, because the emulated device only perceives
   instruction counts and timer ticks, never real elapsed time. The
   standalone port should do the same from day one - free-run instruction
   execution, drive everything off internal cycle-count state - rather
   than needing MAME's overclock-plus-compensating-output-sample-rate
   hack (`providers/doubletalk.py` in `rusty_tts` has the exact mechanism
   if useful as a reference, but it's a workaround for reusing an
   existing hardware-clock-based driver config, not something the
   standalone port should need at all).
2. **Full embedded command set confirmed** from the real manual
   (`docs/dtdoc/Manual.txt`, this repo) - not just Speed. Command format
   `<Ctrl+A><1-2 digit param><letter>`, absolute or relative (`+n`/`-n`,
   wraps at range boundary). Full table (letter, range, default) in
   `notes/standalone_port_prep.md`: Voice `nO` (0-7), Articulation `nA`
   (0-9, default 5), Expression `nE`/`E` (0-9, default 5), Monotone `M`,
   Formant `nF` (0-9, default 5), Speed `nS` (0-9, default 5), Pitch `nP`
   (0-**99**, default 50 - proves the parser handles 2-digit params, so
   Speed's 0-9 ceiling is a genuine firmware design limit not a parsing
   limit), Volume `nV` (0-9, default 5), Tone `nX` (0-2, default 1),
   Reverb `nR` (0-9, default 0). Worth implementing the whole set at once
   in the standalone port - cheap now that the wire format's confirmed,
   real feature surface for rusty_tts callers.
3. **Real TTS input buffer is 3K**, not the 200-char cap
   `providers/doubletalk.py` currently uses (an arbitrary MAME-path safety
   margin, not a hardware limit). Standalone port/provider should support
   up to ~3KB per request.
4. **The ~100ms full-scale DAC startup click is genuine hardware
   behavior** (confirmed - the real card does this too, consistent with
   its DC-coupled bridge-tied output spec), not a MAME artifact. The
   standalone port should *reproduce* this on cold start as a sign the
   init sequence is faithful, not suppress it - though a downstream
   provider will still want to trim it the same way
   `providers/doubletalk.py` does now (skip ~150ms before silence-
   trimming), since it's not meaningful speech content either way.
5. Confirmed hardware specs from the manual worth having on hand: CPU
   Intel 10MHz 80C188EB, 512K ROM + 8K RAM, TTS 3K input buffer, LPC
   synth 4K buffer/TMS5220+D6 formats/8kHz/2 speeds, PCM synth 8-bit
   mono/4K buffer/100 rates 4-11kHz/0-48kHz non-buffered mode, no
   IRQ/DMA/memory requirements - just 2 polled 8-bit I/O ports, jumper-
   selectable among 6 fixed bases (25E/F, 29E/F, 2DE/F, 31E/F, 35E/F,
   39E/F; rusty_tts uses 25E/F).

## Where the code should land

Matching the existing `native/retrochip/` pattern in `rusty_tts`:
- New CPU core + board wrapper source files in `native/retrochip/`
  (alongside `votrax.cpp`, `sp0256.cpp`, `tms5220.cpp`, `tms5110.cpp`,
  `s14001a.cpp`) - hand-ported, dependency-free, no MAME linkage.
- Wire into `native/retrochip/main.cpp` (the shared CLI these all attach
  to) as a new mode.
- `Dockerfile` already compiles retrochip as a single `g++` multi-file
  invocation (no cmake/Makefile) - see the `builder` stage, ~line 41-48 -
  the new source files just need adding to that command's file list.
- Rewrite `providers/doubletalk.py` to shell out to `retrochip` instead of
  MAME, following the pattern of whichever existing provider already
  drives retrochip (check `providers/votrax.py`/`providers/snspell.py` for
  the current convention) rather than the MAME-subprocess pattern it uses
  today.

## Repos/paths involved

- `rusty_tts` (this session's main work): `native/retrochip/` (port target),
  `native/retrochip/doubletalk_notes/PORTING.md` (CPU-port technical ref),
  `providers/doubletalk.py` (MAME-based provider, to be reworked),
  `native/mame-doubletalk/` (vendored MAME driver source - useful as a
  cross-check reference even after the standalone port exists, since it's
  the validated ground truth to diff behavior against).
- `daiverd/mame-doubletalk` (private, branch `doubletalk`, local checkout
  at `/home/ds32/src/mame`): the actual MAME fork with the working driver,
  `investigations/doubletalk-audio-path.md` (RE history/findings),
  `scripts/run_doubletalk_regression.py` +
  `scripts/doubletalk_regression_declaration.lua` (the regression test to
  validate the standalone port against), `DOUBLETALK.md`.
- `daiverd/doubletalk-pc` (private, branch `doubletalk`, this repo, local
  checkout at `/home/ds32/src/doubletalk-pc`): `notes/phase1_findings.md`
  (original ROM/CPU identification RE), `notes/standalone_port_prep.md`
  (this session's findings, detailed version of the summary above),
  `docs/dtdoc/Manual.txt` (real RC Systems manual - full command
  reference, hardware specs; has some non-UTF-8/CP437 bytes partway
  through, use `LC_ALL=C grep -a` or a `latin-1`-decoding reader, not
  plain UTF-8 tools, or matches silently fail past that point), `driver/`
  (Linux `dtlk.c`/`dtlk.h`/`speakup_dtlk.c` reference), `rom/` (the ROM
  dump itself).

## Open items / risks not yet resolved

- Exact DAC output sample rate for TTS mode specifically isn't fully
  pinned down - `PORTING.md` estimates ~10.5kHz from the ~952-cycle timer
  math, described there as "verify, don't assume."
- Real RAM size (8K per manual) vs. the current MAME driver's generous
  untrimmed placeholder (up to `0x1FFFF`) - matters for accurate
  standalone memory-map modeling, not yet narrowed down since nothing in
  the MAME driver's validated behavior has hit whatever the real ceiling
  is.
- Dictionary/phoneme table in the ROM is lightly obfuscated/compressed
  (`notes/phase1_findings.md`), not reverse-engineered to readable form -
  not a blocker (the port executes ROM code, doesn't need to read the
  dictionary directly) but flagged as a known unknown.
- ROM is proprietary, not redistributable - already true for the MAME
  path (mounted read-only at runtime, not baked into the Docker image);
  applies identically to the standalone port.
