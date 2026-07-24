# Phase 1 Findings — DoubleTalk PC ROM Reconnaissance

## Sources used
- ROM dump: `doubletalkpc.BIN`, 524288 bytes (512KB), md5
  `8c0f7a3bd294652486007e163c203434` (proprietary RC Systems firmware — supply your
  own; see LICENSING.md).
- Linux kernel driver: `drivers/char/dtlk.c` + `include/linux/dtlk.h` (pulled from kernel
  tag v6.12/v4.19, since the driver was removed from current `master` in the 7.2 cycle),
  and `drivers/accessibility/speakup/speakup_dtlk.c`.
- RC Systems official manual: `DoubleTalk PC/LT User's Manual` (dtdoc.zip, plain text,
  Copyright 1991-1997) — from rcsys.com/Downloads/dtdoc.zip.
- RC8650/RC8660 chipset manuals (rc8650.pdf, rc8660.pdf, 2014/2015) — downloaded but not
  yet deeply reviewed (PDF text extraction tooling wasn't available in this environment;
  see "Open items" below).

## ROM identification
- Embedded string at file offset `0x1c7e`: `"DoubleTalk (C) 1988-95 RC Systems"` — confirms
  this is the genuine DoubleTalk PC firmware, vintage matches the 1990s ISA card.
- The image is one monolithic 512KB blob, not several discrete chip dumps concatenated —
  there's a single contiguous code region at the start, a large pronunciation/exception
  dictionary in the middle-to-late regions (lots of English words: ABBREVIATE, ANNIVERSARY,
  CONSIGNEE, CONSONANT, CONTAGIOUS, CONVERSATION, COOPERATIVE, ...), and what appear to be
  phoneme/allophone mnemonic tables near the start (`AEIOUY`, `BDVGJLMNRWZ`, letter-adjacency
  tables). Dictionary text is stored under a light obfuscation/compression (letters appear
  case- and pattern-shifted, e.g. runs like `uNSPmR`, `hRPond{`), consistent with a
  space-saving encoding rather than encryption — not a blocker, just needs its own
  reverse-engineering pass if we ever want readable dictionary contents.
- The manual states DoubleTalk has "a built-in, 16-bit microprocessor, 520 Kbytes of
  on-board memory" — 520KB is a good match for a 512KB ROM plus a small amount of banked
  RAM/registers, and matches the 16-bit finding below.

## CPU architecture: confirmed x86-family, 16-bit real mode
This was the main open question in the brief, and it's now answered with high confidence,
**not** TSP5220, 8051, 68HC11, or Z80:

- File offset `0x7FFF0` (= physical `0xFFFF0` if the ROM is mapped into the **top** 512KB of
  a 1MB real-mode address space, i.e. `0x80000–0xFFFFF`) contains:
  ```
  BA A4 FF       MOV DX, 0FFA4h
  B8 00 80       MOV AX, 8000h
  EE             OUT DX, AL
  EA 00 00 00 80 JMP FAR 8000:0000
  ```
  `0xFFFF0` is the **standard 8086/80186/80188 reset vector location**. This is exactly the
  layout you'd expect from a real x86-family reset: a port write (bank-select /
  chip-select setup, port 0xFFA4 looks like an internal peripheral register, not a
  standard ISA port) followed by a far jump.
- That far jump lands at physical `0x80000`, which is **file offset 0** — i.e., the very
  first bytes of the dump are the actual entry point:
  ```
  33 C9          XOR CX, CX
  E2 FE          LOOP $          ; tight ~64K-iteration power-on delay
  BA A8 FF       MOV DX, 0FFA8h
  B8 02 95       MOV AX, 9502h
  8A C4          MOV AL, AH
  B4 10          MOV AH, 10h
  EE             OUT DX, AL
  B8 00 00       MOV AX, 0
  8E D8          MOV DS, AX
  8E C0          MOV ES, AX
  ...
  E6 40          OUT 40h, AL     ; PIT-style port, seen a bit further in
  ```
  This disassembles cleanly as valid, sensible 8086/80186 code (register init, segment
  setup, stack setup, calls into subroutines) — I verified with `objdump -m i8086` over the
  whole image. About 23% of the file disassembles as invalid opcodes when read linearly,
  but that's expected and not a red flag: large parts of the 512KB are the dictionary/data
  tables described above, not code, and linear (non-flow-following) disassembly will always
  hit garbage when it drifts into data.
- Practical implication: the reset/bank-select dance (`OUT` to `0xFFA4`/`0xFFA8` before the
  far jump) suggests the ROM is bank-switched into the CPU's 1MB address space rather than
  fully linearly mapped — i.e. this is very likely an **80188** or **80186**-class embedded
  design (common for ISA co-processor cards of that era because of built-in DMA/timer/chip-
  select peripherals), not a plain 8088/8086 with full linear addressing. This should be
  treated as a working hypothesis to refine once we trace more of the boot code, not a
  final answer.

## I/O port protocol (from Linux driver — treat as ground truth, it's a working real-world implementation)
- Candidate ISA base addresses (jumper-selectable), from `dtlk_portlist[]` in `dtlk.c`:
  `0x25e, 0x29e, 0x2de, 0x31e, 0x35e, 0x39e`. Each card occupies **2 ports**:
  - `LPC port` = base address
  - `TTS port` = base + 1
- **TTS port status flags** (read):
  | bit  | mask | name | meaning |
  |------|------|------|---------|
  | 7 | 0x80 | TTS_READABLE | a byte is available to read from the TTS port |
  | 6 | 0x40 | TTS_SPEAKING | SYNC — nonzero while producing TTS/PCM/CVSD/tone output (not LPC) |
  | 5 | 0x20 | TTS_SPEAKING2 | SYNC2 — falls to 0 up to 0.4s before speech actually stops |
  | 4 | 0x10 | TTS_WRITABLE | RDY — port ready for a byte; goes 0 for 2-3us after write, back to 1 ~180-190us later |
  | 3 | 0x08 | TTS_ALMOST_FULL | AF — <300 free bytes in TTS input buffer (always 0 in PCM/TGN/CVSD) |
  | 2 | 0x04 | TTS_ALMOST_EMPTY | AE — <300 bytes remain in input buffer (always 1 in TGN/CVSD) |
- **LPC port status flags** (valid only after an LPC speak command):
  | bit | mask | name | meaning |
  |-----|------|------|---------|
  | 7 | 0x80 | LPC_SPEAKING | TS — LPC synthesizer producing speech |
  | 6 | 0x40 | LPC_BUFFER_LOW | BL — hardware LPC buffer (4096B total) has <30 bytes left |
  | 5 | 0x20 | LPC_BUFFER_EMPTY | BE — LPC buffer ran dry (error if TS also set) |
- **LPC speak command bytes** (written to select decode table/rate):
  `0x60` = 5220-format table, normal rate; `0x64` = 5220-format, fast rate;
  `0x20` = "D6"-format table, normal rate; `0x24` = D6-format, fast rate.
  **This confirms the TSP5220 connection from the brief** — but as a *data format* the
  firmware can decode (TMS5220-compatible LPC bitstream), not necessarily evidence of a
  discrete physical TSP5220 chip on this board revision. The "D6" format is a second,
  RC-Systems-proprietary LPC format alongside it. Given everything (TTS engine, LPC decode,
  dictionary) lives in one 512KB ROM driven by the 16-bit CPU we identified above, this
  revision looks like it emulates/decodes 5220-style LPC data in firmware rather than
  hosting a separate TI chip — but this is inference, not confirmed; flagging per your
  "don't guess about extra physical chips" instruction.
- Clear/reset: writing `0x18` (Ctrl-X) to the TTS port stops speech (`DTLK_CLEAR`).
- Driver reads settings via an "Interrogate" ioctl whose response layout
  (`struct dtlk_settings`) exactly matches the ASCII command protocol's `?` (Interrogate)
  command described in the RC Systems user manual (serial number, ROM version string,
  mode, punctuation level, formant freq, pitch, speed, volume, tone, expression, dict
  status, free RAM, articulation, reverb, end-of-block marker, indexing support flag) —
  driver and manual **agree** here, no conflict found.

## Command protocol (from RC Systems DoubleTalk PC/LT User's Manual, 1991-1997)
- ASCII, command-character-driven protocol written to the TTS port: commands are of the
  form `<param><command-char>`, e.g. `nS` sets speed, `nP` sets pitch, `nV` volume, `nX`
  tone, `nB` punctuation filter, `nF` formant frequency, `nR` reverb, `nA` articulation,
  `nE`/`E` expression, `nO` voice, `nY` timeout delay, `nQ` sleep timer, `nG` protocol
  options, `nI` index marker, `n*` DTMF generator, `nJ`/`J` tone generator, `T`/`nT` text
  mode, `C`/`nC` character mode, `D` phoneme mode, `#`/`n#` PCM mode, `?` interrogate,
  `L`/`U` load/enable exception dictionary, `@` reinitialize, `Z` zap commands.
  Full command summary table is at the end of that section of the manual if we
  need exact parameter ranges later.
- This manual does **not** cover ISA-specific hardware detail (jumper/base-address
  selection, register bit layout) — it's the software/command-protocol layer, written to
  be portable across DoubleTalk PC (ISA) and DoubleTalk LT (serial). For port-level detail
  we're relying on the Linux driver, which is a real working implementation and should be
  trusted over any marketing-oriented material.

## Disagreements / gaps found
- None yet between the Linux driver and the 1990s user manual — they describe two
  different layers (hardware I/O vs. command protocol) that agree wherever they overlap
  (the Interrogate/settings structure).
- Have **not yet** cross-checked the RC8650/RC8660 chipset manuals in detail — those are
  dated 2014/2015 and almost certainly describe a *later-generation* RC Systems chipset
  (used in current V8600A/V-Stamp/PC-104 products), not the original 1988-95 silicon in
  this ROM. They're useful for the command-protocol family (RC Systems appears to have
  kept it compatible across product generations) but I would **not** trust them for
  register-level/port-address specifics on this specific card revision without flagging
  that explicitly. Environment here lacks a PDF text extractor (no `pdftotext`, no working
  `pip`), so I haven't pulled text out of them yet — doable next if useful, e.g. by writing
  a small manual PDF-stream inflate script, or you could tell me not to bother since the
  Linux driver is the more trustworthy source anyway.

## Phase 3 addendum — debugger trace resolved the "bank-switching" mystery
Got the onboard CPU actually running under MAME (`i80186` core, ROM at `0x80000-0xFFFFF`)
and captured a full instruction trace of the boot sequence via the debugger `trace` command
(driven through MAME's Lua console interface, since this environment is headless — see
`doubletalkpc.cpp` git history / conversation for the working invocation). Two things fell
out of that:

1. **The 0xFFA4/0xFFA8 writes flagged as "bank-switching, needs investigation" in the
   original Phase 1 write-up are not custom card hardware at all.** They are the Intel
   80186's own **on-chip Chip-Select Unit** registers (PACS at PCB-relative offset 0xA4,
   MMCS at 0xA8 — confirmed against MAME's `i186.cpp`/`i186.h`, which already fully models
   these as `I80186_PACS`/`I80186_MMCS` state registers with dedicated read/write handlers).
   This is completely standard 80186 boot-time initialization, already handled correctly by
   MAME's existing CPU core with zero extra work needed from us. The earlier "likely
   bank-switched ROM" hypothesis in the original findings is superseded — no bank-switching
   needed at all, given the 512KB ROM already fits exactly in the CPU's upper 512KB of
   real-mode address space.
2. **The boot sequence completes cleanly and reaches a stable idle state**: firmware runs
   through RAM/variable initialization, a couple of table-building loops, and several
   internal init subroutines, ending in `HLT` at `80101h` — a normal "idle, waiting for
   interrupt" state for an embedded controller, not a crash or a stall. This is strong
   evidence the CPU core and our ROM/RAM memory map are both correct.

**Newly identified real hardware, needing actual emulation (not already covered by the CPU
core)** — these are genuine `OUT` instructions to ports outside the CPU's internal
Peripheral Control Block (0xFF00-0xFFFF), i.e. they reach our (currently unmapped)
`cpu_io()` map and need real handlers:
- **Port 0x40**, written twice (`AL=04h` early in boot, `AL=[1Ah]` — a runtime variable —
  later in a routine that also does `CLI`/`OR [1Ah],10h`/`POPF`). Port 0x40 is the classic
  8253/8254 PIT channel-0 data port in PC-compatible designs. Given the manual's mention of
  "tone generators" and the fact this card needs to *generate audio timing itself*, this is
  almost certainly a genuine discrete timer chip on the card (not the 80186's internal
  timers, which live in the PCB range and weren't touched here) — plausibly driving
  tone/pitch generation or a baud/sample-rate clock.
- **Port 0x80**, written `AL=7Fh`. Port 0x80 is the well-known PC "POST diagnostic code"
  port by convention; could be a genuine debug/diagnostic output on this card, or coincidence
  since it's also just a fast dummy I/O delay port on real PCs — needs more tracing (e.g. a
  breakpoint-driven trace once more of the boot sequence executes) before assuming either.
- **Port 0x00**, written `AL=80h` immediately after the port 0x80 write. Port 0 is
  conventionally the channel-0 address register of an 8237-style DMA controller in
  PC-compatible designs — plausible if this card DMAs LPC/PCM sample data internally, though
  this is inference, not confirmed.

None of this required us to guess — it's all directly observed CPU behavior from a real
instruction trace, cross-referenced against MAME's own (accurate, pre-existing) 80186 model.

## Phase 3 addendum #2 — interrupt wiring confirmed, text-buffering still open
Continuing from the "genuine external ports still need real handlers" list above:

- **INT0 confirmed as a working periodic wake source.** i80186 external interrupt
  pins are masked by default (I0CON/I1CON reset to 0x000f) and the traced boot path
  never unmasks them itself, so the driver now force-unmasks I0CON on every tick.
  There's a real implementation gotcha here worth remembering: pulsing an interrupt
  line with `assert` immediately followed by `clear` in the *same* synchronous call
  does nothing, because `i80186_cpu_device::external_int()` only reacts to an actual
  state *change*, and no CPU cycles elapse between the two calls to let the dispatch
  loop see the pending flag - it has to be a real square wave (or, for one-shot
  doorbell-style signals like INT1 below, assert immediately then deassert via a
  short one-shot timer, not synchronously).
- Also needed: the traced ISRs (`8000:1E2A` for INT0, `8000:1D26` for INT1) return
  via plain `IRET` with **no software EOI**, and the i80186 core's in-service bit
  only clears on an explicit EOI write - without one, only the very first interrupt
  of a given type is ever delivered, confirmed empirically (trace stayed flat after
  one ISR execution despite continued pulsing). The driver now forces a
  non-specific EOI (`0x8000` → port `0xff22`) after every pulse. Real hardware
  presumably has an auto-EOI mode enabled somewhere we haven't found, or unmasks/
  EOIs through a code path this ROM's specific boot sequence doesn't take - this is
  an engineering workaround, not a confirmed-accurate reproduction of real hardware.
- **INT1 confirmed as the host "byte arrived" doorbell — this is the strongest
  finding of the session.** Disassembling its handler at physical `0x81D26` shows
  it reading the incoming byte from a **fixed memory-mapped address, `0xA100`**,
  then comparing it against `0x18` — exactly `DTLK_CLEAR` (Ctrl-X) from the RC
  Systems manual - and jumping to the reinitialization routine on a match. This
  isn't inference; the vector was genuinely installed by the boot code, pulsing it
  genuinely lands there, and the constant it checks against genuinely matches the
  documented protocol. `doubletalkpc.cpp`'s `host_w()` now writes each host TTS-port
  byte to `0xA100` and pulses INT1 to match.
- **What's still open:** writing a real 5-letter word ("HELLO") through this path
  produces no observable change in the `0x9500-0x953F` RAM region or the `[7h]`
  mode byte. Re-reading the INT1 handler disassembly explains why: for any byte
  that *isn't* `0x18`/`0x19` (the two cases it special-cases), it just falls through
  to a generic `pop ax; mov word ptr [9502h],8000h; iret` - i.e. this handler's
  job looks like minimal interrupt housekeeping (handle the two control bytes,
  write one fixed marker word) rather than the actual "append this character to
  the text buffer" logic. That logic is presumably elsewhere - either read back
  out of `0xA100` again during the main polling loop (background/foreground split
  between ISR and main loop is a common embedded pattern), or gated behind the
  `[0ADh]` bit-0x80 "channel busy" flag the handler tests, or something not yet
  traced. Finding it needs either more targeted tracing of the main loop's
  branches (particularly the `[7h]`-mode-dependent dispatch and the two
  channel-control-block checks at `[0Fh]`/`[13h]` described below) or accepting
  this needs substantially more systematic reverse engineering than trial-and-error
  interrupt-line guessing.
- One dead end worth recording so it isn't retried: the two "channel control
  block" structures set up during boot (flag byte + word at `0x14A0`/`0x149E` and
  at `0x1352`/`0x1350`, handler-offset-looking values `0x2080`/`0x2020`) are **not**
  code pointers - `0x82080` disassembles as pronunciation-dictionary data, not
  code. Whatever those words mean, it isn't "jump here."

## Phase 3 addendum #3 — interrupt delivery bug found and fixed (root cause, not a workaround)
The "text sent through INT1 produces no visible effect" finding above turned out to
rest on a broken foundation: interrupt delivery itself was silently failing the entire
time, for a different reason than the masking/EOI issue already described.

**Root cause**: MAME's `i80186_cpu_device` intercepts internal Peripheral Control Block
register accesses (chip-select unit, interrupt controller mask/EOI registers, timers)
as a special case *inside its own instruction-execution path* - the C++ code that runs
when the CPU itself executes an `IN`/`OUT` instruction. Writing to those same port
addresses from *outside* the CPU (via `m_cpu->space(AS_IO).write_word(...)`, which is
how a sibling device like ours would normally poke a CPU's port space) goes through the
*generic* address map instead - which is empty in `cpu_io()` - and never reaches that
interception logic at all. So every "unmask I0CON/I1CON" and "force an EOI" write the
driver was doing was a **complete no-op**, confirmed by reading `I0CON` back afterward
and finding it still `0x000F` (masked) no matter how many times the "fix" ran.

This is why the very first debugger-trace test (Lua directly poking
`cpu.state["I0CON"].value = 0`) worked, while every later attempt to replicate that
from the driver's own C++ using address-space writes silently failed: the Lua
`.value = ...` assignment goes through `device_state_interface`'s by-name state lookup,
a completely different (and, for internal-only registers like this, the *only* working)
path than the address map.

**Fix**: added `set_cpu_state(name, value)` to the driver, which does the exact same
by-name lookup over `state_entries()` that MAME's Lua console uses under the hood, and
switched the I0CON/I1CON unmask and the EOI (implemented as directly clearing `INSERV`,
since there's no separate state entry for the EOI register write itself) to use it.
Confirmed fixed by direct before/after readback (`I0CON` reads `0000` after the fix,
`000F` before) and by a fresh debugger trace: a 6-second run with one host byte written
partway through produced **57,761 trace lines with 959 combined hits on the INT0
(`81E2A`) and INT1 (`81D26`) vector addresses**, versus the flat 221-line (boot-only,
zero interrupts) trace every "confirmed working" test after the original one-shot Lua
test had actually been silently producing.

**Net effect on the earlier "no visible reaction to HELLO" finding**: re-run with
interrupt delivery genuinely fixed, a full 64KB memory diff before/after sending
"HELLO" *still* shows only `0xA100` changing (the host's own write reflecting back).
This is no longer ambiguous - interrupt delivery is proven reliable by the trace
evidence above, so this result now cleanly confirms rather than confounds the earlier
disassembly-based conclusion: for any byte that isn't `0x18`/`0x19`, INT1's handler
really does just do minimal fixed housekeeping (write a constant marker word that
INT0's own tick was already writing too, hence no visible diff) and return - the real
"append this character to the speech queue" logic genuinely lives somewhere else
(main loop, or a routine not yet traced), and finding it is no longer clouded by any
doubt about whether the interrupt substrate itself works.

## Phase 3 addendum #4 — INT1 delivery fixed for real, and text is now genuinely queued
The earlier "interrupt delivery is fixed" claim (addendum #3) turned out to only be
true for INT0. A closer look at that same trace (separating the combined grep into
per-vector counts) showed **zero** INT1 (`81D26`) hits even in the "confirmed working"
test - the 959 hits were all INT0. INT1 had never actually fired once, in any test,
despite `I1CON` correctly reading back as unmasked.

Root cause, found via targeted diagnostic logging: the original design deasserted INT1
via a fixed 200us one-shot timer (mirroring INT0's "must be a real edge" lesson). But
`i80186_cpu_device::update_interrupt_state()`'s dispatch loop checks external interrupt
sources in a fixed `int_num` order (INT0 before INT1) *within the same priority level*,
and **returns from the whole function early** if it finds an in-service interrupt at
that priority - it never reaches INT1's check at all when that happens. Since INT0 and
INT1 defaulted to the same priority (0) after unmasking, and INT0 is constantly
toggling, the 200us window could easily land entirely within a stretch where
`update_interrupt_state()` kept bailing out early on INT0 - and the timer-driven
deassert would then clear INT1's still-unserviced request bit before a later call ever
got a chance to notice it. Confirmed by holding INT1 asserted continuously (level-style,
deasserting only once `mailbox_r()` - the ISR's own confirmed first instruction -
actually ran) instead of a timed pulse: **still never delivered**, even held for 100ms+.

**Actual fix**: give INT1 a higher priority than INT0 (`I0CON` set to priority 1,
`I1CON` stays priority 0) so it can never lose a same-priority race in that dispatch
loop. Reasonable on its own terms too - a "the host is telling you something" doorbell
outranking a generic periodic housekeeping tick isn't a stretch. Combined with the
read-triggered deassert (removes the timing-guess race entirely) and the earlier
by-name state-lookup fix (addendum #3), INT1 now delivers reliably.

**Confirmation this actually closes the original question**: with all of the above
fixed, a full 64KB memory diff before/after sending "HELLO" through the host TTS port
now shows real, meaningful changes for the first time:
```
[14A0]-[14A5]: 80 00 00 00 00 00  ->  48 45 4C 4C 4F 80    ("HELLO" + 0x80 terminator)
[0011]:        A0                ->  A5                    (write pointer, +5 for 5 chars)
[1346]-[1347]: 00 00             ->  A0 14                 (little-endian 0x14A0 - buffer base?)
[000A]:        00                ->  26                    (unclear - possibly a counter/checksum)
[A100]:        00                ->  4F                    (the mailbox itself, last byte written)
```
`0x14A0` is exactly the "channel A" control-block address identified back in the very
first boot trace (flag byte initially `0x80`, per the `812DA` init routine) - this
closes the loop on that earlier open question too: it's a real text buffer, not (or not
only) whatever the "handler offset" field turned out to be. **The host-to-firmware text
path is now confirmed working end to end**: host writes a byte -> mailbox + INT1 ->
ISR reads it -> firmware appends it to a real growing buffer with an advancing pointer.

**What's still not done**: this only proves text reaches an internal buffer, not that
speech gets produced or reaches an audio output - the buffer could just be sitting
there until whatever timing condition or IRC- or the LPC channel's decode step
processes it, which hasn't been traced yet. No DAC/speaker device exists in the driver
at all yet either. Both remain open next steps toward actual "speech out."

## Phase 3 addendum #5 — found and proved the consume-pointer mechanism, natural trigger still unknown
Following up on "text is queued but never processed further" from addendum #4:

Traced the main polling loop's `800C6` check specifically for the case where the
buffer's flag byte is no longer `0x80` (i.e. real text is present). It leads to
`807E6h`: `mov ax,[0Fh]; cmp ax,[17h]; je 80818h` (early return if equal) - i.e. this
compares two word variables at fixed low addresses `0x0F` and `0x17`, both of which
were initialized to the same value (`0x14A0`, the buffer base) during boot and
**never observed to change on their own** afterward, in any amount of run time tried
(confirmed by grepping every trace captured this session for a write to `[0Fh]` -
found exactly one, the boot-time initialization).

Directly tested the hypothesis that `[0Fh]` is a "read/consume" pointer by nudging it
forward by one byte via the debugger (`pspace:write_u8`, no rebuild needed) after
sending "HELLO.": **this worked** - the buffer visibly shifted left by one position
(`48 45 4C 4C 4F 2E 80` → `45 4C 4C 4F 2E 80 80`), i.e. the leading `H` was consumed
and the remainder moved down, and `[0Fh]` itself snapped back to `0x14A0` afterward
(consistent with the firmware treating `0x14A0` as "the front" post-shift). This is
solid, direct proof `[0Fh]` really is a genuine consume-pointer wired into real
buffer-processing logic, not a dead end.

A second experiment - nudging `[17h]` to match the write pointer `[11h]` instead -
produced a different and harder-to-interpret result (the firmware appeared to
*duplicate* "HELLO." into the buffer again rather than consume it), which is more
likely an artifact of poking the emulator into a state real firmware logic never
actually visits than a second legitimate control path - flagging it as inconclusive
rather than a finding.

**Net status**: the buffer-consumption logic itself is real and does the right thing
when triggered. What's still unknown is what's *supposed* to trigger `[0Fh]` forward
under normal operation - none of the sources wired so far (INT0's housekeeping tick,
INT1's mailbox doorbell) touch it directly in anything traced. Plausible candidates
not yet tried: the still-unwired LPC port (offset 0) - the manual's Speak commands
(`0x60`/`0x64`/`0x20`/`0x24`) might need to be issued to arm a shared output stage
before the TTS engine's own consume step runs; or a currently-unmapped port (`0x00`,
`0x80` - see earlier addenda) that might carry a "DAC/output ready" signal from real
audio hardware, itself needed to legitimately pull the next character. Both require
new, careful tracing rather than more ad-hoc pointer nudging, which risks producing
misleading results by pushing the emulated state somewhere real firmware never goes.

## Phase 3 addendum #6 — CR trigger confirmed (per manual!), real synthesis engine engages, then crashes in dictionary decode
Per the DoubleTalk PC/LT User's Manual ("TTS Operating Modes" /
"Text mode" section, not previously read closely): **"DoubleTalk will not begin
speaking until it receives a CR (0Dh) or Null (00h) character - this ensures that
sentence boundaries receive the proper inflection. This is the default operating
mode."** We had been sending punctuation but never a CR/Null - that's exactly why
addendum #5's buffered text never got consumed.

Tested immediately: sent "HELLO" through the confirmed-working mailbox/INT1 path,
then a CR (`0x0D`). Result: **`[0Fh]` (consume pointer) jumped straight to match
`[11h]` (write pointer)** - the whole buffered sentence got consumed at once, matching
the manual's own description of Text mode speaking "complete sentences." This
directly, empirically closes out addendum #5's open question - no LPC-port arming or
further guessing needed, just reading the manual for something the user correctly
suspected must be documented.

**What happens next is real, and it's exciting**: `[0Fh]`/`[11h]` move on to
`0x8281`/`0x8282`, execution speed drops to ~25-50% (heavy computation), and a
targeted trace shows genuinely legitimate ROM-reading code: a bit-rotation/unpacking
routine at `0x84C04` that computes `ES = 0x9672 + cx` (a real ROM segment - physical
`0x96720+`, well inside the mapped ROM region) and reads compressed data via
`lodsw es:[si]`, in a tight loop that runs for ~2000 instructions repeatedly across
many INT0 tick interruptions. This is genuinely RC Systems' proprietary pronunciation-
rule/dictionary decompression algorithm engaging with real, correctly-loaded ROM data -
not a hallucination, not garbage-as-code. **The synthesis engine is real and it runs.**

It then computes a far call target from decoded data and jumps to `6C49h:2052h` -
`CS=0x6C49` is not a valid code segment (confirmed via register dump: this really did
land there, corrupting `CS`/`IP` and everything downstream). Sending phonemes directly
via Phoneme Mode (`^AD` command, bypassing the text-to-phonetics translator per the
manual) reached the *exact same* `0x8281`/`0x8282`/crash pattern regardless of input
content - meaning this specific crash point is shared low-level buffer-processing
infrastructure common to both modes, not something specific to word-lookup content,
and Phoneme mode doesn't sidestep it as hoped.

**Working theory, not yet confirmed**: something upstream of the `84C04` decode
routine - likely a one-time table/index setup we haven't triggered, possibly RAM state
real hardware would have from a self-test or LPC-channel arm step - is feeding it (or
a routine shortly after it) bad data, causing a correctly-executed decode to compute a
nonsense call target. The `ES=0x9672+cx` ROM access itself looks legitimate; the bug
is somewhere in what happens with the decoded result afterward. No evidence found of a
missed bank-switch/chip-select reconfiguration near the crash (checked - no writes to
the `0xffa4-0xffaa` chip-select range appear nearby), so that hypothesis is likely a
dead end, not the cause.

**Assessment**: this is now a well-localized but genuinely deep bug in reverse-
engineering RC Systems' proprietary dictionary/rule compression format - a
substantial sub-investigation in its own right, not a quick fix. Given how far this
session has already gone (full host-to-firmware pipeline working, confirmed real
synthesis engine engagement), this is a natural point to check in on how much further
to push before audio output is reachable.

## Phase 3 addendum #7 — collaborator (ctoth) root-caused the crash and got real audio working
After addendum #6 left off (dictionary-decode crash, deep and unclear), collaborator
`ctoth` picked up the investigation and made the actual breakthrough — full detail is in
the companion MAME driver's investigation notes; this is a summary.

**The crash was ours, not the firmware's.** The "bad far-call target" from addendum #6
wasn't a dictionary-decoder bug at all: our own synthetic 1kHz INT0 timer (an engineering
workaround from addendum #3, invented because we didn't understand the real interrupt
source) re-entered a firmware copy loop mid-instruction and corrupted a register. Deleting
that synthetic timer's deassert-side EOI (letting the firmware own its own interrupt
lifecycle again) made the crash disappear entirely.

**The CPU was misidentified.** Not a generic 80186 — the RC Systems PC/104 hardware
manual (an upward-compatible derivative) specifies a **10MHz 80C188EB**, a variant with a
*relocatable* Peripheral Control Block. Boot code writes `0x1095` to the relocation
register, which correctly decoded moves the peripheral block to physical `0x9500` —
retroactively explaining several addresses earlier addenda had been treating as ordinary
RAM. This closes out the "bank-switching" blocker below: it was never bank-switching,
it was chip-select relocation on a CPU variant we hadn't identified.

ctoth implemented a proper `I80C188EB` CPU subtype directly in MAME's core
(`src/devices/cpu/i86/i186.cpp`/`.h`, not just the driver file) — a real emulator
contribution, not a local hack. With the correct CPU model, the firmware's own interrupt
handling and EOI work naturally, and all of our `set_cpu_state`/forced-priority/forced-
INSERV workarounds from addenda #3–#4 became unnecessary and were deleted.

**Real audio confirmed.** Port `0x00` turned out to be an unsigned 8-bit PCM DAC output,
driven by the CPU's own (now-correctly-modeled) timer ISR. Routed to a MAME DAC + speaker
and recorded to WAV.

I independently verified this rather than just trusting the commit messages: rebuilt from
the exact committed state, reran the deterministic `HELLO` trace myself (`bad_transfer:
false`, converged pointers, valid `CS`), and inspected the WAV directly — confirmed a real
2-channel recording (motherboard speaker / DoubleTalk) with a genuine, bounded speech-
shaped burst on the DoubleTalk channel only, silent on the other, matching ctoth's
documented burst timing almost exactly.

**Then I listened to it.** Sent "HELLO" through — clean, clear synthesized speech, no
garbling. Sent a long multi-sentence passage (opening of the Declaration of Independence,
503 bytes across two sentences) — this was the first real test of anything beyond a
single short word, and it also worked: all bytes sent, no crash, buffer fully drained,
~19 seconds of clear, continuous speech across both sentences.

Two apparent oddities in that longer recording both turned out to be test-harness/host
artifacts, not card problems, confirmed by direct channel-by-channel inspection:
- **~3.5s of lead-in silence** in the raw session recording — this is our test script's
  own startup + cautious per-byte RDY-gated send pacing, not present once the audio is
  properly trimmed to where real content starts (~0.25s in).
- **A "beep" mixed into the middle of a word** when listening to the raw two-channel
  mix — genuinely the *host PC's own motherboard speaker* (confirmed on channel 1, e.g.
  peaks around 8.7–8.9s in one run, plus a small blip at boot — likely a GLaBIOS/BIOS
  event), not the DoubleTalk card's audio at all. Channel 1 is silent everywhere the
  DoubleTalk channel has speech; the "mixing" was purely coincidental timing when both
  channels get downmixed together by a naive player.

**Regression test added** (in the companion MAME driver repo: `scripts/run_doubletalk_regression.py`
+ `scripts/doubletalk_regression_declaration.lua`): sends the same long Declaration
passage and asserts both (a) no crash — `CS` stays `0x8000`, all bytes sent, buffer
pointers converge — and (b) real audio actually comes out (a sustained, non-trivial-
amplitude burst on the isolated DoubleTalk channel, not silence). Verified the test
actually catches each of those failure modes (corrupted CS, stuck send, undrained buffer,
silent audio) before relying on it, not just that it passes on the happy path. Runs in
~4 seconds.

**Where this leaves things**: "text in, speech out" — the original Phase 3 goal — is
achieved and independently verified, for both short and long input. What's not yet
established is *hardware-accurate* fidelity (real RAM size vs. our 128KB placeholder,
real CPU clock, LPC port wiring, jumper-selectable base address, Interrogate response,
comparison against a real card) — see ctoth's `docs/reports/doubletalk-operational-
assessment.md` for the full remaining roadmap ("Gates 2 onward") if that level of
accuracy becomes the next goal.

## Blockers / risks for later phases
- No blocker on ROM completeness — this looks like a single self-contained image (code +
  both LPC formats' decode logic + dictionary), so I don't currently see evidence we need a
  separate physical chip dump. I'll flag immediately if deeper RE turns up a reference to
  external LPC coefficient ROM content that isn't present in this image.
- ~~Bank-switching behavior (writes to 0xFFA4/0xFFA8 before the entry far-jump) means a
  correct emulator/MAME driver will need to model whatever chip-select/paging hardware
  sits behind those ports, not just a flat ROM.~~ **Resolved (addendum #7): not
  bank-switching — the CPU is an 80C188EB with a relocatable Peripheral Control Block,
  and 0xFFA8 is its relocation register (`RELREG`).**
