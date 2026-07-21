# DoubleTalk PC: path to fully operational MAME emulation

Date: 2026-07-21

## Question

What is required to turn the current `doubletalk-pc` research repository and
the `mame-doubletalk` scaffold into a fully operational, 100% DoubleTalk PC
emulation?

## Scope and method

This began as a read-only source and runtime assessment. It covered:

- the firmware, manuals, historical host drivers, utilities, disassembly, and
  chronological findings in `doubletalk-pc`;
- the DoubleTalk ISA device, MAME integration, ROM packaging, build guide,
  MAME 80186 implementation, and comparable 80186 devices in
  `mame-doubletalk`;
- live build attempts using both the original and corrected commands in
  `DOUBLETALK.md`;
- official RC Systems, Linux driver, and MAME documentation available online.

The follow-up build-readiness slice installed the required Debian development
packages, produced the named `doubletalk` binary, and launched the real
`pcv20`/`doubletalkpc` configuration successfully. No device behavior was
changed in that slice.

## Executive finding

The current MAME code is a reverse-engineering scaffold, not an operational
emulation. It compiles as an individual object, but it cannot presently be
detected by a real DoubleTalk host driver, return an Interrogate response,
produce audio, or reproduce most of the documented device modes. The recorded
firmware crash is downstream of an inaccurate and incomplete board model, so
it is not yet evidence that the proprietary dictionary decoder itself is bad.

The correct route is deletion-first foundation repair: establish a reproducible
MAME build and hardware oracle; replace the guessed memory, interrupt, and
mailbox behavior with observed board behavior; prove the firmware executes
naturally through speech generation; then add the real audio/status/LPC/PCM/
CVSD/tone devices. Adding an HLE synthesizer, adapter, or more state-forcing to
the present scaffold would not produce 100% operation.

## Verified facts

### Reference repository

- `rom/doubletalkpc.BIN` is exactly 524,288 bytes.
- MD5: `8c0f7a3bd294652486007e163c203434`.
- SHA-256:
  `7629885bd2ea5a9eb533a8f229240c23560d7226f1f38d990804e53fee860b39`.
- The reset vector and coherent boot code establish an x86-family embedded CPU
  compatible with the 80186/80188 family.
- The official manual specifies a built-in 16-bit microprocessor, 512 KiB ROM,
  8 KiB RAM, TTS, LPC, PCM/ADPCM, CVSD, and multiple tone generators.
- The official developer documentation says some units can have 8 KiB or
  32 KiB RAM. The exact ROM/board revision under emulation must therefore be
  matched to actual hardware rather than inferred from the ROM size alone.
- The ISA host sees two adjacent eight-bit I/O ports. DoubleTalk PC uses no
  host IRQ or DMA. Internal onboard CPU interrupts/DMA are a separate matter.
- Text mode requires CR (`0x0d`) or NUL (`0x00`) before synthesis begins.
- The repository includes the original `TEST.COM`, `DTINFO.COM`, `SMARTALK`,
  `DTPRN.COM`, Windows drivers, LPC samples, PCM/ADPCM samples, tone files, and
  exception dictionaries. These are valuable black-box acceptance artifacts.

### MAME repository and build

- The checkout is MAME 0.288 with the DoubleTalk device registered as ISA
  option `doubletalkpc`.
- The branch has no usable upstream history. Commit `467cb88a` is a squashed
  root snapshot containing the entire MAME tree plus the scaffold. The exact
  upstream tag `mame0288` exists at `2c38dc6e555e17560bbf6f5531c3e86cf8570f54`.
- The original `DOUBLETALK.md` command failed after about nine minutes because
  SDL2/SDL2_ttf development headers were missing and GCC 12.2 diagnostics were
  promoted to errors.
- The required SDL2, SDL2_ttf, Fontconfig, and PulseAudio development packages
  are now installed in WSL Debian.
- The corrected named-subtarget command completed successfully twice:

  `make -j$(nproc) SUBTARGET=doubletalk SOURCES=src/mame/pc/genpc.cpp REGENIE=1 SYMBOLS=0 USE_QTDEBUG=0 NOWERROR=1`

- The resulting `doubletalk` ELF reports MAME 0.288. A finite headless launch
  loaded `pcv20.zip` and `doubletalkpc_isa.zip`, started
  `:isa6:doubletalkpc` and its 80186, ran for two emulated seconds, and exited
  successfully.
- `DOUBLETALK.md` now contains the live-verified dependency, build, and smoke
  commands. The smoke command deliberately uses `-sound none`; it proves
  build/device startup, not audio acceptance.

### Current device behavior

- ROM is mapped at `0x80000-0xfffff`.
- RAM is mapped across almost all of `0x00000-0x1ffff` (128 KiB), despite the
  hardware documentation specifying 8 KiB or, for some variants, 32 KiB.
- A host-to-CPU byte mailbox is guessed at `0xa100`; RDY is derived from its
  pending flag.
- CPU clock is guessed at 16 MHz.
- A 1 kHz periodic INT0 source, INT0/INT1 priorities, forced mask-register
  values, and direct clearing of the CPU core's `INSERV` state are engineering
  workarounds, not modeled hardware.
- CPU I/O accesses at `0x00`, `0x40`, and `0x80` are observed but unmapped.
- The MAME 80186 core exposes `chip_select_callback()`. Comparable MAME 80186
  systems bind it to install board peripheral windows dynamically. DoubleTalk
  does not bind it.
- LPC writes are logged and discarded. LPC reads return a static zero.
- TTS SYNC, SYNC2, AF, and AE are static. There is no CPU-to-host TTS output
  latch or host acknowledgement path for Interrogate data.
- Reset returns LPC `0x00` and TTS RDY-only `0x10`, yielding host word
  `0x1000`. Real Linux/Speakup probes require idle `0x107f`, then wait for
  `0x147f`. The scaffold therefore cannot be found by a real driver.
- There is no DoubleTalk sound device, DAC route, speaker, LPC decoder path,
  PCM/ADPCM path, CVSD path, or tone-generator path.
- Device-owned state is not registered for MAME save states.
- No DoubleTalk-specific Lua harness, trace, regression test, host-oracle
  capture, or deterministic end-to-end test is checked in.

## Why the recorded far-call failure is not yet a root cause

The chronological note reports that after `HELLO\r`, the firmware processes
ROM data and ultimately transfers to `6c49:2052` (physical `0x6e4e2`), outside
the mapped ROM. It labels this a bad computed dictionary-decode target.

That conclusion is premature:

- the live trace and Lua procedure that produced it were not preserved;
- the linear disassembly shows the same byte sequence in data-like regions,
  so linear decoding cannot establish the instruction's semantic origin;
- RAM size/decode is knowingly wrong;
- the 80186 chip-select callback is ignored;
- required peripherals and natural interrupt/EOI behavior are absent;
- the CPU is being forced through guessed interrupt states.

The first causal question is whether an accurate memory/peripheral/interrupt
model removes or relocates that transfer. Dictionary-algorithm work is only
authorized by evidence after that foundation passes.

## Missing evidence required for a 100% claim

The repositories do not contain enough hardware evidence to claim exact
emulation. The user reports that recognizable phoneme samples are directly
audible when regions of the ROM are played at roughly 10 kHz. That materially
reduces the audio-content gap once the sample format, table structure, playback
rate, and firmware output path are verified, but it does not yet establish the
board's digital-to-analog or analog behavior. The following evidence remains
desirable for a board revision matching the ROM:

- high-resolution front/back PCB photographs and all chip markings;
- exact CPU variant and oscillator frequency;
- installed RAM size and address decode;
- PAL/GAL/PLD identities and dumps where readable;
- timer/counter, latch, DAC, LPC, CVSD, amplifier, and filter components;
- power-on traces of chip-select registers and external bus accesses;
- onboard CPU interrupt source, polarity, priority, and EOI behavior;
- host-port latch/status/read-ack timing;
- digital audio samples or bus captures for TTS, LPC, PCM/ADPCM, CVSD, and
  tone modes;
- reference recordings and status traces from the official utilities.

The low-resolution photo on RC Systems' legacy page proves the board contains
substantial discrete logic, but its labels are unreadable. If no physical card
or equivalent captures can be obtained, “fully usable with documented HLE” may
be achievable, but “100% hardware emulation” is externally blocked and must not
be claimed.

## Ordered convergence path

Each gate changes a decision about the next gate. Do not begin a later gate
until the previous gate passes.

### Gate 0: trustworthy source and build ledger

1. Reconstruct the DoubleTalk work as a narrow branch based on verified tag
   `mame0288`; keep the squashed-root checkout as evidence, not as the ledger.
2. Preserve only the actual DoubleTalk source/build-registration/ROM metadata
   delta. Do not carry generated build output.
3. Establish one documented build environment with all MAME prerequisites.
4. Correct and live-verify the scoped build procedure, including required
   regeneration. Record its baseline duration and terminal output.
5. Gate: clean build, `-listdevices` exposes `doubletalkpc`, and ROM audit
   accepts both committed ZIPs.

### Gate 1: reproducible oracle before code changes

1. Check in a deterministic Lua harness that cold-boots the card, reads both
   host ports, sends bytes only when RDY is set, sends `HELLO\r`, captures
   onboard CPU state/bus events, and exits MAME with a machine-readable result.
2. Check in the exact trace configuration that reproduces the current bad
   transfer, including the instruction that loads/calls the target and the
   preceding writes to every consumed state location.
3. Run the same host sequences on real hardware using official utilities or a
   logic analyser and preserve normalized captures.
4. Gate: every current claim can be reproduced from a clean checkout; causal
   ordering is explicit.

### Gate 2: replace the guessed board foundation

1. Identify the exact CPU variant/clock and RAM population from hardware.
2. Replace the 128 KiB placeholder RAM with the observed decode.
3. Bind and model 80186 chip selects where the board uses them.
4. Model the actual timer/latch/peripheral windows and natural interrupt/EOI
   behavior.
5. Delete `set_cpu_state`, the synthetic 1 kHz INT0 path, forced priorities,
   and forced `INSERV` clearing once their real owner paths exist. Do not hide
   them behind a helper or adapter.
6. Gate: firmware cold-boots and remains live without debugger pokes or direct
   CPU-private-state mutation; save/load returns to the identical state.

### Gate 3: complete the ISA contract

1. Implement the jumper-selectable base address.
2. Implement actual LPC/TTS latches, CPU-side ownership, host read/ack paths,
   and measured timing.
3. Implement the idle/probe signatures, RDY transitions, SYNC/SYNC2/AF/AE,
   LPC TS/BL/BE, index markers, clear/reinitialize, and Interrogate response.
4. Gate: the historical Linux `dtlk` and Speakup probe/interrogate algorithms
   complete without special cases, and the official DOS detection utilities
   find the device.

### Gate 4: prove firmware execution through synthesis

1. Re-run the preserved CR-trigger trace on the accurate foundation.
2. If the bad transfer remains, trace its data provenance backward to the first
   divergent value and compare that value with hardware.
3. Correct only the owning hardware/model defect. Do not replace the firmware
   translator with a host synthesizer.
4. Gate: text and direct phoneme modes reach legitimate sample/control writes
   repeatedly, with no unmapped execution or forced pointer changes.

### Gate 5: implement every audio engine

1. Model the identified DAC, clocks/DMA, CVSD device, LPC device/data formats,
   PCM/ADPCM buffering, tone generators, mixer, filters, volume control, and
   speaker/headphone route.
2. Drive host status from the real engine/buffer states.
3. Gate each family separately against hardware captures: TTS, phoneme, 5220
   LPC normal/fast, D6 LPC normal/fast, buffered PCM, non-buffered PCM, ADPCM,
   CVSD, musical tones, sinusoidal tones, and DTMF.

### Gate 6: product-level exact convergence

1. Run `TEST.COM`, `DTINFO.COM`, `SMARTALK`, DTPRN, index-marker scenarios,
   exception dictionaries, Spanish data, every included LPC/PCM/TGN sample,
   clear/reinitialize, buffer-full/empty behavior, and all jumper addresses.
2. Verify cold reset, repeated reset, save/load, throttled/unthrottled timing,
   and audible output with sound enabled.
3. Compare port/status timelines and digital audio against real hardware under
   defined tolerances. Any unexplained diff remains an unchecked item.
4. Gate: all documented modes and supplied artifacts pass; no static stubs,
   guessed clocks, forced CPU state, unmapped board I/O, or obsolete parallel
   paths remain.

## Immediate next decision

The reproducible build prerequisite now passes, and implementation is
authorized. The next target is Gate 1: preserve a deterministic text-plus-CR
trace, correlate the reported ROM phoneme regions with firmware reads and
candidate output writes, and use that evidence to identify the first owning
board-model defect. Exact analog fidelity may remain externally uncertain, but
it no longer blocks firmware-derived functional speech work.

## Primary external references

- RC Systems legacy product page: `https://www.rcsys.com/legacy.htm`
- RC Systems download center: `https://www.rcsys.com/dnlds.htm`
- RC Systems DoubleTalk Developer's Tools manual:
  `https://dectalk.nu/Software%20and%20Manuals/Manuals/RC%20Systems/DoubleTalk%20Developers.pdf`
- Linux `dtlk` driver source:
  `https://codebrowser.dev/linux/linux/drivers/char/dtlk.c.html`
- Official MAME build documentation:
  `https://docs.mamedev.org/initialsetup/compilingmame.html`

## Local evidence pointers

- Firmware/manual inventory and chronological trace findings:
  `notes/phase1_findings.md:193`, `notes/phase1_findings.md:292`,
  `notes/phase1_findings.md:337`, `notes/phase1_findings.md:383`.
- Text/phoneme CR requirement: `docs/dtdoc/Manual.txt:1495` and
  `docs/dtdoc/Manual.txt:1511`.
- Hardware memory and audio-mode specifications:
  `docs/dtdoc/Manual.txt:2682` and `docs/dtdoc/Manual.txt:2711`.
- Real host probe, Interrogate, read/ack, and write behavior:
  `driver/dtlk.c:408`, `driver/dtlk.c:510`, `driver/dtlk.c:565`, and
  `driver/dtlk.c:631`.
- Speakup idle signatures: `driver/speakup_dtlk.c:352` and
  `driver/speakup_dtlk.c:368`.
- Scaffold host state and status behavior:
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:39` and
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:54`.
- Placeholder memory and unmapped board I/O:
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:128` and
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:136`.
- Forced interrupt state, guessed clock/tick, and reset values:
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:150`,
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:197`,
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:227`, and
  `../mame-doubletalk/src/devices/bus/isa/doubletalkpc.cpp:254`.
- Current build/run claims: `../mame-doubletalk/DOUBLETALK.md:19` and
  `../mame-doubletalk/DOUBLETALK.md:30`.
- MAME compiler/regeneration requirements:
  `../mame-doubletalk/docs/source/initialsetup/compilingmame.rst:11` and
  `../mame-doubletalk/docs/source/initialsetup/compilingmame.rst:18`.
- MAME Debian prerequisites and `NOWERROR` behavior:
  `../mame-doubletalk/docs/source/initialsetup/compilingmame.rst:302` and
  `../mame-doubletalk/docs/source/initialsetup/compilingmame.rst:625`.
- 80186 chip-select callback and internal register implementation:
  `../mame-doubletalk/src/devices/cpu/i86/i186.h:22` and
  `../mame-doubletalk/src/devices/cpu/i86/i186.cpp:2006`.
- Comparable dynamic 80186 peripheral mapping:
  `../mame-doubletalk/src/mame/cinematronics/leland_a.cpp:408`.
