# Porting DoubleTalk PC to standalone C++ - scope assessment and reference

Status: **done - see `doubletalk/`.** The port took the
"vendor the MAME core verbatim + compatibility shim" route rather than the
hand-transcription this document was bracing for: the five CPU core files
compile unmodified against `shim/emu.h`, so the transcription-bug failure
mode described below never applied. Every reference fact in this document
was validated against the working port (first-HLT checkpoint, RELREG
behavior, 952-cycle timer cadence, read-triggered INT1 deassert). This
file remains useful as the map of *why* the pieces are shaped the way they
are; the shim's own header comments cover the mechanics.

## Why this turned out to be bigger than the initial estimate

The initial plan treated this like the existing `native/retrochip/` chips
(votrax.cpp, sp0256.cpp, etc.) - hand-port ~300-2000 lines, done in one sitting.
On investigation, the CPU core itself is ~2600 (i86.cpp) + ~2200 (i186.cpp)
lines, and unlike a synthesis chip's mostly-arithmetic state machine, a
correct x86 CPU interpreter has a large surface area (every addressing mode,
every flag update, segment overrides, string instructions, etc.) where a
subtle transcription bug produces a program that runs and does *something*
plausible-looking rather than an obvious crash - exactly the failure mode
that cost this project many iterations even with MAME's own mature,
already-correct core (see `mame-doubletalk`'s `investigations/doubletalk-
audio-path.md` and `doubletalk-pc`'s `notes/phase1_findings.md` addenda #3/#4
for the history: an edge-triggering bug, a missing-EOI bug, an address-space-
vs-state-lookup bug, and an interrupt-priority race all individually produced
"looks like it's working" states that weren't).

Recommendation for whoever picks this up: budget this as a real multi-session
project (realistically comparable in size to the original MAME driver
investigation, which took a full extended session with dozens of rebuild-test
cycles), not a quick add-on. Validate *continuously* against the known-good
MAME reference (build it, run `mame-doubletalk`'s
`scripts/run_doubletalk_regression.py`, diff behavior) rather than writing the
whole core and testing at the end.

## What's confirmed and can be used directly (no further RE needed)

Everything below is extracted from currently-committed, working source in the
`doubletalk` branches of `mame-doubletalk` and `doubletalk-pc` - not guesses.

### CPU identity and clocking
- 80C188EB (`I80C188EB` in `src/devices/cpu/i86/i186.h`), a real-mode 8-bit-bus
  member of the 80186 family with a *relocatable* Peripheral Control Block
  (PCB). Clock: 20MHz XTAL / 2 = 10MHz processor clock (see
  `doubletalkpc.cpp`'s `device_add_mconfig`).
- Reset vector at physical `0xFFFF0`; entry code far-jumps to `8000:0000`
  (physical `0x80000`) after a boot-time port write.

### Memory map (from `doubletalkpc.cpp::cpu_map`)
```
0x00000-0xa0ff   RAM (128KB placeholder total up to 0x1ffff - real hardware
0xa100           mailbox_r/mailbox_w (word-aligned bus requirement - EB has
                 a 16-bit bus even though it's the "188" 8-bit-external-bus
                 part; MAME's install requires the region boundary to be
                 word-aligned, hence 0xa100-0xa101 not just 0xa100)
0xa101-0x1ffff   RAM (continued)
0x80000-0xfffff  ROM (512KB, doubletalkpc.BIN, loaded flat, no banking)
                 has 8KB or 32KB - not yet narrowed down or needed for
                 correctness, since nothing has hit the RAM ceiling)
```

### I/O map (from `doubletalkpc.cpp::cpu_io`)
```
port 0x00   write: unsigned 8-bit PCM sample -> DAC. This is the audio
            capture point. Driven by the firmware's internal timer ISR
            once the CPU's own 80C188EB timer peripheral is correctly
            modeled - no external "audio ready" signal needs modeling,
            it falls out of correct timer emulation.
port 0x40   write: complete host-visible TTS status byte (DS:0x1a in
            firmware) - bit layout is exactly TTS_READABLE(0x80)/
            SYNC(0x40)/SYNC2(0x20)/RDY(0x10)/AF(0x08)/AE(0x04) per the
            RC Systems manual and Linux dtlk.c. Firmware owns this
            entirely; the board wrapper just latches whatever's written
            and exposes it to the host read side unchanged.
port 0x80   host-visible LPC status/data latch (ISA base+0). Firmware
            drives it to 0x7f (idle sentinel) once early in boot, and
            writes an index-marker number when speech reaches a Ctrl-A
            <n> I marker. Latched by the board wrapper for both the
            index-marker stream and host_lpc_status(). See the LPC-port
            addendum below.
0xff00-range Peripheral Control Block, see below - owned entirely by the
            CPU core, not board logic.
```

### Host protocol (from `doubletalkpc.cpp::host_r/host_w/mailbox_r/pulse_int1`)
- Host TTS-port write -> `m_mailbox_data = data; m_mailbox_pending = true;`
  then assert INT1 (`m_cpu->int1_w(1)`) and **leave it asserted**.
- Firmware's ISR at ROM address `0x81D26` (physical) reads `0xA100` as its
  first instruction - this is mailbox_r() in the board wrapper. That's the
  point to clear `m_mailbox_pending` and deassert INT1
  (`m_cpu->int1_w(0)`). **Do not use a fixed delay for the deassert** - an
  earlier version tried a timed one-shot deassert and it silently dropped
  interrupts under a same-priority race with INT0's own activity (see
  addendum #4/#7 in phase1_findings.md for the full story). Read-triggered
  deassert is a hard requirement, not a simplification.
- Host TTS-port read -> just returns the latched `m_tts_status` byte
  (written by the firmware itself via port 0x40 - no board-side RDY
  computation needed anymore, unlike an earlier iteration of this driver).
- **No priority-forcing needed** in the current driver at all - the real
  80C188EB peripheral block (once correctly modeled) handles I0CON/I1CON/
  EOI/priority arbitration on its own, naturally, because the firmware
  configures it correctly itself. All of the `set_cpu_state`/forced-
  priority/forced-`INSERV`-clearing workarounds from earlier in this
  project's history are **gone** from the current driver - don't
  reintroduce them in the standalone port. If you find yourself needing
  one, that's a signal your CPU core has a bug, not that the workaround
  is legitimate.

### Text protocol (from `docs/dtdoc/Manual.txt`, `doubletalk-pc` repo)
- Default mode ("Text mode"): speech does not begin until a CR (`0x0D`) or
  Null (`0x00`) byte is received - buffers everything up to that point as
  one utterance. A well-behaved sender polls the RDY bit (`0x10` of the TTS
  status byte) before every write.
- Verified working end-to-end (independently, by ear) with both a single
  short word and a 503-byte multi-sentence passage - see
  `mame-doubletalk`'s `scripts/doubletalk_regression_declaration.lua` for
  the exact byte sequence used (opening of the Declaration of Independence,
  chosen arbitrarily as a long/real test phrase, nothing special about the
  content) and `doubletalk-pc`'s `notes/phase1_findings.md` addendum #7 for
  the listening-test writeup.

## Peripheral Control Block reference (the hard part)

Source: `i80186_cpu_device::internal_port_offset/internal_port_r/
internal_port_w/update_interrupt_state/handle_eoi/external_int/
restart_timer/internal_timer_sync/inc_timer/internal_timer_update/
timer_elapsed/int_callback` in `src/devices/cpu/i86/i186.cpp` (this exact
revision is on the `doubletalk` branch of `mame-doubletalk`, commit history
includes ctoth's `I80C188EB` additions - diff against `torvalds`-equivalent
upstream MAME if you want to isolate exactly what was added for EB support
vs. pre-existing 80186 logic).

### Relocation register (RELREG)
- I/O port `0xFFA8` at reset (`m_reloc` defaults to `0x00ff` for EB parts,
  `0x20ff` for plain 80186/80188 - **use the EB default**, `0x00ff`).
- Bit 14: iRMX mode selector - **not used by this firmware** (confirmed
  `RELREG` capture during boot showed `0x1095`, bit 14 = 0). Don't bother
  implementing the iRMX branches in `update_interrupt_state`/`handle_eoi`/
  `external_int` at all - dead code for our purposes, skip it to cut scope.
- Bit 12 (`0x1000`): PCB mapped in memory space vs. I/O space. Firmware
  value `0x1095` has this bit clear -> I/O-space PCB (matches the observed
  `0xFF00`-range port writes). Only implement the I/O-space path.
- Low byte (`reloc & 0xff`): the I/O page the PCB lives at - relocated PCB
  occupies `(reloc & 0xff) << 8` through `+0xff` in port space. With
  `RELREG=0x1095`, PCB moves to I/O page `0x95xx`... **cross-check this
  against the actual observed writes before trusting it** - the driver
  comment says "physical 0x9500" for the relocated PCB, which reads more
  like a *memory* address than an I/O port page; re-derive this precisely
  from `read_port_byte`/`write_port_byte`'s exact masking logic
  (`(port >> 8) == (m_reloc & 0xff)`) rather than trusting my summary,
  since getting this wrong makes literally everything else fail silently.

### Register offset translation (EB mode only - `internal_port_offset`)
EB byte-offsets (what the firmware actually writes) translate to the
"classic 80186" word-offsets used internally. Word-offset = byte-offset/2.
Verified table (EB byte offset -> classic-186 register):
```
0x00 int vector      0x01 EOI            0x02 poll        0x03 poll status
0x04 int mask        0x05 priority mask  0x06 in-service  0x07 request
0x08 int status      0x09 timer0 intctl  0x0c INT0 ctl    0x0d INT1 ctl
0x0e INT2 ctl        0x0f INT3 ctl       0x18 timer0 cnt  0x19 timer0 cmpA
0x1a timer0 cmpB     0x1b timer0 ctl     0x1c timer1 cnt  0x1d timer1 cmpA
0x1e timer1 cmpB     0x1f timer1 ctl     0x20 timer2 cnt  0x21 timer2 cmpA
0x23 timer2 ctl      0x54 relocation reg
(everything else -> generic 128-word m_eb_registers[] scratch array,
 read back as written, no side effects - safe to implement as inert
 storage, matches what real firmware apparently never depends on for
 anything we've observed)
```
(EOI at byte offset `0x01`*2=`0x02` matches the driver header comment
"issues its own EOI through offset 0x02".)

### Interrupt dispatch (`update_interrupt_state`, non-iRMX path only)
Priority-ordered (0 = highest, checked first) scan across: timer0 (fixed
slot), DMA0/DMA1, then INT0-INT3 external. For each source at the current
priority level being scanned:
- If already in-service at that priority (`in_service & irq_bit`) with no
  pending upgrade -> **return immediately, stop scanning entirely** (this
  is the priority-race behavior that broke things in the MAME driver's own
  development history - a lower-priority source will never even be looked
  at while a higher one is in service, by design, not a bug to work around).
- Else if requested (`request & irq_bit` or, for timer0, `status & 0x07`)
  -> compute vector (external: `0x0c + int_num`; timer0: `0x08`/`0x12`/
  `0x13` depending on which of the 3 sub-conditions), set `ack_mask`,
  assert the CPU's `INTR` line (once - `pending` flag guards against
  re-asserting if already pending).
- Vector fetch on acknowledge (`int_callback`): clears `request &
  ack_mask` (unless level-triggered/`LTM` for external - **verify our
  firmware doesn't set `LTM`**, i.e. it's edge-triggered, matching the
  "stays asserted until read-triggered deassert" host protocol already
  described above - if LTM logic is needed here too there's a discrepancy
  worth investigating, not assuming away), sets `in_service |= ack_mask`,
  returns `vector | priority` for timer0 or `poll_status & 0x1f` otherwise
  -> used as `vector * 4` into the real-mode IVT for the far-call.
- EOI (`handle_eoi`, non-iRMX): **specific EOI** (`data` bit 15 clear) uses
  a fixed table mapping vector byte -> in-service bit to clear (`0x08/
  0x12/0x13`->timer0 bit `0x01`, `0x0a/0x0b`->DMA bits, `0x0c-0x0f`->
  `0x10<<int_num` for INT0-3). **Non-specific EOI** (`data==0x8000`, which
  is what this firmware's confirmed-real `write [reloc_pcb+0x02]=0x8000`
  pattern uses per the driver's header comment) walks priority 0-7 and
  clears the first in-service bit found, in the same priority order as
  dispatch.

### Timers (audio-critical - this is where port 0x00's PCM stream comes from)
Three timers (0/1/2), each with `count`/`maxA`/`maxB`/`control` (16-bit
each). Control register bits (from `internal_timer_update`/`inc_timer`):
- bit15 (`0x8000`): timer enabled/running.
- bit13 (`0x2000`): interrupt-on-max enabled.
- bit12 (`0x1000`): ALT - alternate between counting up to maxA then maxB
  (vs. always maxA) when bit1 (`0x0002`, continuous mode) is set.
- bit1 (`0x0002`): continuous (auto-reload) vs. single-shot.
- MAME schedules a real-time event `4 * (target - count)` cycles out via
  `emu_timer` (`cycles_to_attotime`) - **the standalone port should instead
  track this as a plain integer "cycles remaining" countdown per timer,
  decremented by however many cycles the interpreter's step loop consumes,
  firing the equivalent of `timer_elapsed()` at zero** - don't try to
  replicate MAME's event-scheduler abstraction, a simple per-instruction
  (or per-N-instructions, batched for speed) cycle-budget decrement is
  simpler and sufficient here since we don't need MAME's general-purpose
  multi-device scheduling.
- `timer_elapsed(which)`: sets control bit `0x0020` (max-count-hit flag),
  and if bit13 set, sets `m_intr.status |= 1<<which` and calls
  `update_interrupt_state()` - this is what generates the actual interrupt
  that the ISR at ROM `0x81C9F` (per the investigation doc) services to
  emit each PCM byte to port 0x00.
- Given confirmed real-world PCM cadence is ~10.5kHz and CPU clock is
  10MHz, expect the firmware to program the timer for roughly
  `10,000,000 / 10,500 ≈ 952` cycles between interrupts - useful as a
  sanity check once the timer port-write trace is captured from a real
  run (verify, don't assume this number is exactly what the firmware
  programs).

## Suggested next-session plan
1. Get a minimal general 8086/80186 real-mode instruction interpreter
   working first, headless (no board at all) - validate it can at least
   execute the ROM's boot sequence up to the first `HLT` at physical
   `0x80101` and match the *exact* instruction trace already captured and
   preserved in this project's history (see `full_disasm.txt` and the
   trace excerpts embedded in `phase1_findings.md` addenda). This gives a
   cheap, concrete, early correctness checkpoint before touching
   interrupts/timers/audio at all.
2. Add the PCB/interrupt/timer subsystem from the reference above, and get
   INT1 (host doorbell) working - validate against sending a single short
   phrase and confirming the same buffer-pointer-advancement behavior
   already documented (e.g. `[14A0]` filling with the sent bytes).
3. Add timer-driven INT0 and port-0x00 capture, get real audio out, and
   validate against the long-phrase regression test's pass criteria (see
   `mame-doubletalk`'s `scripts/run_doubletalk_regression.py` - the same
   phrase, the same two checks: no crash, real sustained audio).
4. Only then wire into `retrochip`'s CLI and write the Python provider.

## Addendum: the LPC status/detection port (base+0, CPU port 0x80)

The MAME driver kept a separate `m_lpc_status` latch initialized to 0x00 and
noted "the LPC port's CPU-side wiring is still unconfirmed (not yet traced)".
That has now been traced empirically in the standalone port. The host-visible
LPC port (ISA base offset 0) is the same **CPU I/O port 0x80** already used
for index markers:

- During boot the firmware writes **0x7f to port 0x80 once** (traced at
  ~t=1.05M cycles, right as RDY comes up). 0x7f is the idle sentinel Linux's
  `drivers/char/dtlk.c` expects: `dtlk_readable()` is `inb_p(lpc) != 0x7f`,
  and `dtlk_dev_probe()` only recognizes the card when the 16-bit word read
  at the base port satisfies `(word & 0xfbff) == 0x107f` - low byte 0x7f
  (LPC idle) and high byte 0x10 (TTS RDY bit). So **the 0x7f is firmware-
  driven, not open-bus / pulled-up data lines.**
- When speech reaches a `Ctrl-A <n> I` index marker the firmware writes the
  marker number to the same port (e.g. 0x05), which is how the LPC port
  doubles as the index-mark readout path (`dtlk_read_lpc`: any value != 0x7f
  is an available byte; the host writes 0xff back to acknowledge and the
  firmware restores 0x7f).

The board wrapper now latches every port-0x80 write into `m_lpc_status`
(seeded to 0x7f at reset as a pre-boot default) and exposes it via
`doubletalk_board::host_lpc_status()` and the C API `dtalk_lpc_status()`,
so a future ISA-port shim can present a probeable card to DOS/Linux screen
readers. The existing index-event stream is unchanged (the marker writes are
still emplaced into `m_index_events`).
