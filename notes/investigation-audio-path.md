<!--
license:BSD-3-Clause
copyright-holders:Christopher Toth
-->

> **Provenance:** written by Christopher Toth during the reverse-engineering of
> the companion MAME DoubleTalk PC driver. The reproduction commands below run
> inside that MAME driver environment, not this standalone repo. Imported here
> as reference; see also `notes/phase1_findings.md` and `notes/PORTING.md`.

# Investigation: DoubleTalk firmware-to-audio path

## Objective

Make the firmware accept a host phrase, complete synthesis without invalid
execution, and drive a MAME audio device with the ROM-derived phoneme stream.

## Facts (verified)

- The named `doubletalk` MAME 0.288 subtarget builds and launches `pcv20` with
  the committed GLaBIOS and DoubleTalk ROM ZIPs.
- The onboard 80186 boots to a stable idle loop at physical `0x80101`.
- Host TTS bytes written at ISA `0x25f` reach the firmware through the modeled
  `0xa100` mailbox and INT1 path.
- Sending `HELLO` advances the firmware write pointer and places
  `48 45 4c 4c 4f 80` at physical `0x14a0`.
- CR (`0x0d`) is the documented and observed natural consume trigger.
- After CR, both text mode and direct phoneme mode reach the same reported bad
  transfer to `6c49:2052` (physical `0x6e4e2`).
- A newly preserved full instruction trace proves that reported far call is not
  the first divergence. Execution is already decoding data as instructions at
  `0x87a93`; the `call 6c49:2052` at `0x87aa0` is one downstream garbage
  instruction. The earlier claim that legitimate dictionary code computed that
  far target was wrong.
- The current device maps placeholder RAM through most of `0x00000-0x1ffff`,
  while the manuals specify 8 KiB and some developer material mentions 32 KiB
  variants.
- External onboard CPU ports `0x00`, `0x40`, and `0x80` are unmapped.
- Periodic INT0 frequency, interrupt priorities, forced unmasking, forced
  `INSERV` clearing, and the 16 MHz CPU clock are not hardware-confirmed.
- There is no DoubleTalk sound device, DAC, or speaker route.
- The user reports recognizable phonemes when ROM regions are interpreted at
  approximately 10 kHz. The exact offsets, encoding, and rate are not yet
  verified in this checkout.
- Port `0x40` writes are sourced from firmware byte `DS:0x1a`. Its manipulated
  bits are exactly the documented TTS status bits: SYNC/SYNC2 (`0x60`), RDY
  (`0x10`), AF (`0x08`), and AE (`0x04`).
- The routine at `0x81d10` loads a byte through a moving buffer pointer, writes
  it to port `0x00`, advances/wraps the pointer, and returns from interrupt.
  Boot writes `0x80` to the same port, consistent with unsigned 8-bit PCM
  midpoint/silence.

## Theories (plausible)

1. Missing external audio-ready/timing behavior prevents the firmware's shared
   synthesis consumer from advancing correctly. This predicts meaningful
   access to ports `0x00`, `0x40`, or `0x80` around the first bad transfer.
2. Placeholder RAM or omitted chip-select decoding corrupts a shared table or
   pointer. This predicts the bad target is derived from state written outside
   the documented RAM decode or from an incorrectly aliased address.
3. The reported target is a trace/disassembly interpretation error. A clean
   deterministic run will not trap physical `0x6e4e2`, or will show a different
   causal source.
4. The bad target is valid encoded data being consumed as a far pointer because
   a missing initialization step selected the wrong mode/table. This predicts
   the immediate pointer provenance is stable across runs and shared by text
   and phoneme modes.

## Tests Run

| Test | Hypothesis | Result | Rules Out | Supports |
|------|------------|--------|-----------|----------|
| Verified named build and two-second device launch | Build/runtime prerequisite | Pass | Toolchain and ROM packaging as current blockers | Investigation can run from a clean checkout |
| Historical `HELLO` plus CR trace | 1-4 | Reported common bad transfer after real buffering and consume | Host mailbox and missing CR as root cause | Shared low-level synthesis-path defect |
| First deterministic harness run | Harness validity | Failed before emulation: 80186 I/O write taps used one-byte ranges, but MAME requires the full two-byte native span | No firmware hypothesis | Tap endpoints must be `port` through `port + 1` |
| Harness with corrected tap spans and bad-target breakpoint | Harness validity | MAME terminated with SIGSEGV before producing a result; no core file was preserved | No firmware hypothesis | Breakpoint/stop-exit path or I/O tap path must be isolated |
| Same harness with only the bad-target breakpoint disabled | 1-4 and harness validity | Pass: sent all six bytes and exited deterministically; final CS was `0x6c49`, pointers were `0x8281/0x8282`, and port `0x40` writes changed from repeated `04,04,14` groups during byte input to `64,74` after CR | I/O taps or normal scripted exit as the SIGSEGV cause; theory 3's prediction that the historical segment would not recur | Unsafe debugger stop/exit interaction caused the harness crash; port `0x40` participates in the active phrase path |
| Passive `CS=0x6c49` detection without debugger control | 1-4 and harness validity | Pass with exit 0 and one `DOUBLETALK_RESULT`; all six bytes were sent, the invalid segment was detected, and the same pointer and port-`0x40` sequence was captured | Debugger support as a requirement for the baseline | The shared bad segment is reproducible in an ordinary headless run |
| Exact passive baseline repeated | Determinism | Identical result including CS/IP/PC, all three pointers, 16 buffer bytes, and the complete timestamped I/O event sequence | A transient interrupt race as the immediate cause of the observed baseline | The harness is stable enough to gate device changes |
| Disassemble exact I/O writer PCs | 1 and audio-path identification | Port `0x40` writes `DS:0x1a`, whose bit operations exactly match documented TTS status; port `0x00` receives moving-buffer bytes and is initialized to `0x80` | Port `0x40` as a generic PIT data port; port `0x00` as merely a PC-style DMA address register | Port `0x40` is the TTS status latch; port `0x00` is the primary sample DAC path |
| Map port `0x40` to host TTS status and delete synthetic mailbox-derived RDY | 1 | Build passes; unchanged harness accepts all six bytes through firmware RDY and reproduces the identical invalid-transfer result | Missing/incorrect host TTS status as the cause of the bad transfer | Firmware status ownership is correct but causally upstream-independent of the remaining failure |
| Preserve full onboard CPU trace through the failure | 2-4 | Legitimate looping remains in `0x84cxx`, then execution appears at data-like `0x87a93`; the apparent far call is later at `0x87aa0` | The far call itself as the first/root divergence; legitimate code computing `6c49:2052` | The first bad control transfer targets `0x87a93` or an earlier nearby data address and must be traced at its source |
| Trace without loop condensation and tap INT0 vector writes | Synthetic INT0 corruption | IRQ0 interrupts the copy loop at `0x84c52`; handler `0x81e62` sets `CH=2` without preserving `CX`; repeated ticks prevent `LOOP` from finishing, `0x84c58` wraps its destination through the vector, and the final vector is `8283:7f81` (physical `0x87a93`) | Dictionary decoding, chip selects, RAM aliasing, and the later far call as causes of this failure | The guessed periodic INT0 schedule is the direct cause of invalid execution |
| Inspect MAME 80186 external-interrupt request and acknowledge path | Whether the CPU core itself re-enters INT0 incorrectly | `external_int()` latches an external request and `update_interrupt_state()` suppresses the same source while its `in_service` bit is set; the driver timer explicitly clears `INSERV` on deassert, making every 1 ms pulse independently serviceable | An unconditional 80186-core re-entry defect as the immediate cause | The board driver's invented timer/`INSERV` manipulation defeats the core's normal same-source suppression |
| Inspect 80186 execution-loop delivery conditions | Whether external requests bypass firmware interrupt masking | The core dispatches maskable IRQ only when `m_IF` is set and normal no-interrupt latency permits it; external INT0 ultimately uses that path | External delivery bypassing `IF` or instruction-boundary protection | Re-entry is caused by the board model's repeated serviceable requests, not by the CPU core ignoring firmware masks |
| Inspect DoubleTalk interrupt lifecycle | Whether another modeled device or firmware-visible action acknowledges INT0 | No other modeled INT0 source/acknowledgment exists. The timer deassert half clears the entire `INSERV` state, and mailbox consumption also clears the entire state rather than only INT1 | A hidden modeled acknowledgment that makes the 1 kHz source safe | The current INT0/INT1 lifecycle is synthetic and incorrectly coupled; a source-level experiment must isolate removal of recurring INT0 serviceability |
| Delete only deassert-side synthetic INT0 EOI, rebuild, and run unchanged phrase harness | Recurring serviceability causes the corruption | Pass: build exits 0; all six bytes are consumed; `bad_transfer:false`; vector stays `1e2a:8000`; read/write pointers converge at `0x14a6`; execution remains in ROM at `0x814ee` | A necessary legitimate transfer to `0x87a93`; another independent cause of the low-vector overwrite in this trace | Synthetic EOI deletion is a measured kept improvement; real sample-event acknowledgment/rate remains to be modeled |
| Disassemble stable PC `0x814ee` and INT0 handler `0x81e2a` | Stable PC exposes an event wait predicate | Refuted for `0x814ee`, which is a plain `RET` sampled after drain. INT0 instead reads `DS:0x9900`, requires `(value & 0x58) == 0x58`, consumes one byte from `SS:SI`, and writes it to `DS:0x9900`; otherwise it sets `CH=2` | `0x814ee` as a hardware wait loop; port `0x00` as the only plausible sample sink | Address `0x9900`, currently placeholder RAM returning zero, is a missing status/data hardware register on the INT0 sample path |
| Search full ROM disassembly for `0x9900` access family | Other accesses initialize or acknowledge the same device | Executable matches are limited to INT0 and boot code at `0x8024a` that derives configuration bytes from literal `0x9900`; later textual matches lie in sample/table regions decoded as instructions | A broad foreground polling/acknowledgment family elsewhere in executable ROM | Boot configuration plus the INT0 read/write pair define the missing device boundary |
| Disassemble boot routine around `0x8024a` | Boot code directly initializes the `0x9900` device | It performs no access to `0x9900`; it builds seven four-byte entries at `0x9580`-`0x959b`, deriving four entries from `0x9900 >> 4`/`+0x4a` and three from zero/`0x48` | A direct device reset/configuration write at boot | The generated table encodes address/segment metadata whose consumers must identify the real role of `0x9900` |
| Search executable ROM for absolute consumers of generated `0x9580` table | The table has direct code references that identify its type | No executable absolute consumers were found; reported later matches are data/sample regions decoded linearly as instructions | A simple direct foreground consumer | The table is consumed indirectly, so runtime segment/register evidence at INT0 is required |
| Capture first post-CR INT0 registers with an auto-continuing debugger tracepoint | Resolve displacement `0x9900` to physical hardware | `DS=0000`, `SS=002d`, `SI=24a6`, `DI=149f`; the status/data access is physical `0x09900` and the candidate sample read is physical `0x02776` | `0x9900` resolving into ROM through a nonzero data segment | Physical `0x9900`, currently ordinary RAM, is the missing status/data boundary |
| Inspect first post-CR INT0 trace neighborhood | Determine branch and interrupted context | INT0 interrupts legitimate parsing at `0x81068`; the RAM-backed `0x9900` read fails the `0x58` mask, executes `CH=2`, and returns | The ready/sample branch already operating with the placeholder map | Correct `0x9900` read semantics are required before recurring sample interrupts can be enabled |
| Inspect other executable `0x58` comparisons | Determine whether `0x58` is a shared hardware-ready constant | Other comparisons at `0x80530` and `0x84a47` are character/parser handling; the apparent `0x816e9` match is inline data | A broader firmware status API using the same mask | The `0x58` ready test is local to the INT0 device access |
| Inspect RC Systems PC/104 hardware manual and ISA product photo | Constrain compatible hardware without guessing from firmware alone | The upward-compatible PC/104 derivative specifies a 10 MHz 80C188EB, 8 KiB RAM (32 KiB optional), a 4 KiB PCM buffer, and 8-bit programmable 4-11 kHz rates. Its simplified output is low-pass filtered. The official ISA photo is too low-resolution to read chip labels | The driver's 16 MHz clock as documented; a high-rate or 16-bit PCM path | A 10 kHz-class unsigned 8-bit stream is hardware-plausible, but the derivative manual does not by itself prove the ISA board's exact CPU subtype or register implementation |
| Return fixed ready mask `0x58` at physical `0x9900`, capture writes, and leave recurring INT0 disabled | The one-shot ready branch exposes a synthesized byte | Rejected: build and execution pass safely, but seven writes are all `0x00` and occur at boot/after mailbox-driven global `INSERV` clears. The register map alone services INT0 at the wrong times and does not expose synthesized audio | A fixed-ready register plus the current interrupt lifecycle being sufficient for coherent output | Physical `0x9900` is on the branch, but INT0 must be gated to the firmware's prepared sample-playback context; the source experiment must be restored |
| Enumerate and disassemble all executable port-`0x80` writes | Port `0x80` gates or acknowledges sample service | Six writes form a control family: boot/stop `0x7f`; PCM setup and low-water `0xdf`; `0xa500` ring high-water `0x9f`; and values below `0x64` streamed by another ISR. The same ISR family accesses missing physical device `0xa500` | Port `0x80` as a passive POST/delay port or primary byte DAC | Port `0x80` is a multi-mode hardware control latch coordinating several external data/interrupt paths |
| Map boot vectors and dynamic handler changes | External inputs correspond to the decoded device windows | INT0/type `0x0c` installs `0x81e2a` (`0x9900` sample output); INT1/type `0x0d` installs `0x81d26` (`0xa100` host mailbox); INT2/type `0x0e` installs `0x81dd1` and later `0x81d8f` (`0xa500` producer/capture). Internal timer vector `0x08` installs `0x81c9f` and drives port `0x00` | One generic periodic interrupt serving all sound paths | The firmware has distinct external sample-output, host-mailbox, and producer/capture sources plus an internal timer path |
| Inspect MAME relocated internal-peripheral decode and EOI handler | Whether firmware write `[0x9502]=0x8000` can be the natural interrupt acknowledgment | If `RELREG` selects memory page `0x95`, physical word write `0x9502` decodes to internal offset `0x11`; MAME would call `handle_eoi(0x8000)` and re-run interrupt arbitration | No runtime premise; this is a conditional core capability | Live `RELREG` must be captured before treating the firmware write as EOI |
| Capture live `RELREG`, interrupt controls, and `INSERV` after phrase | Whether the conditional relocated-EOI decode is active | Refuted: `RELREG=0x20ff`, so physical `0x9502` does not reach the core's internal offset `0x11`; final `INSERV=0x0010`. `I0CON=0x0001` and `I1CON=0x0000` are forced by the driver; `I2CON/I3CON=0x000f` remain masked | The active firmware naturally EOIing through MAME's relocated internal window; immediate deletion of all driver acknowledgment | The conditional core capability exists but is inactive in this runtime; `0x9502` must be classified before changing acknowledgment behavior |
| Decode reset `AX=0x1095` against the primary 80C186EB/80C188EB register definition | Whether the firmware targets a base-80186 chip-select register or the EB peripheral relocation register | MAME correctly places full AX on the internal bus for `OUT DX,AL`, but the current base-80186 model misdecodes port `0xffa8` as MMCS. On an EB at reset, `0xffa8` is PCB offset `0xa8`, `RELREG`; `0x1095` relocates the PCB into memory at `0x9500` | The firmware programming base-80186 MMCS or unrelated external board logic | The firmware expects an 80C188EB-compatible peripheral block at physical `0x9500`; the current CPU subtype is wrong |
| Decode physical-`0x9502` writes against the EB peripheral block | Whether the firmware has a natural interrupt acknowledgment mechanism | EB EOI is PCB offset `0x02`, so word `0x8000` at relocated physical `0x9502` is non-specific EOI. EB interrupt controls at offsets `0x18` through `0x1e` also align with firmware writes at `0x9518` through `0x951e` | Physical `0x9502` as unrelated software/board state | MAME needs EB register layout and semantics; the driver-side `INSERV` manipulation compensates for the wrong CPU model rather than missing firmware acknowledgment |
| Search MAME CPU devices for an existing 80C188EB-compatible peripheral block | A reusable EB/EM implementation already exists and should own the register semantics | Refuted: the only enhanced 80188-class type is `AM188EM`; it merely changes clock-to-cycle conversion and otherwise inherits the incompatible base-80186 register map. No EB/EC type or PCB offset-`0xa8` implementation exists | Reusing `AM188EM` as though it implemented EB peripherals; creating a duplicate of an existing owner | A new 80C188EB CPU subtype must own relocation and PCB register translation; the DoubleTalk driver should select that literal CPU rather than emulate its internal registers as board logic |
| Implement an `I80C188EB` subtype with EB reset relocation and PCB-to-existing-controller register translation | The existing controller semantics are sufficient for the firmware once accesses reach the right registers; firmware EOI will replace driver state mutation and a phrase will drain without invalid execution | Confirmed: exact build exits 0; trace reports `RELREG=0x1095`, `INSERV=0`, firmware-owned `I0CON=3`, `I1CON=0`, `I2CON=2`, intact vector, drained `14a6/14a6` pointers, and no invalid transfer. Removing the synthetic INT0 timer and state mutation is safe | The base-80186 model, driver-forced control state, and synthetic periodic INT0 source | The EB core slice restores the firmware's own interrupt lifecycle and activates its internal timer path |
| Observe external output after EB timer activation | Port `0x00` is the firmware PCM byte stream once the correct internal timer executes | Confirmed: after phrase acceptance, timer ISR `0x81c9f` emits a dense stream from `0x81ca9`, centered at unsigned silence `0x80` with sample variation including `0x7f` and `0x81`; this activity did not exist under the wrong CPU model | Port `0x00` as only inert delay/housekeeping output | Route the literal byte stream to an unsigned 8-bit DAC; do not synthesize samples from ROM separately |
| Route port `0x00` to MAME's unsigned 8-bit R-2R DAC and record the deterministic phrase | The firmware-derived stream produces a non-silent WAV with speech-shaped energy at its native timer cadence | Confirmed: exact build and clean trace pass. A minimal-slot recording is 48 kHz 16-bit PCM; a card-absent control identifies channel 1 as the motherboard speaker, leaving channel 2 unambiguously DoubleTalk. The card channel spans `-16710..8430`, RMS `-24.73 dB`, and contains a compact speech-shaped burst from `0.401` to `0.638` seconds followed by silence | Port `0x00` as non-audio control data, wrong polarity, or requiring another output gate | Keep the direct unsigned DAC route; document the isolated run/record/extract command and preserve a verified card-only WAV |

## Current Best Theory

The invalid-execution root cause is established and removed: the guessed
periodic INT0 source re-entered a firmware copy loop after the handler changed
`CH`; deleting the driver's invented deassert-side EOI prevents repeat service,
keeps the vector intact, and lets the phrase buffer drain normally. The
dictionary decoder, memory map, and later far call are not causes of this
failure. The earlier identification of port `0x00` as the primary sample output
was obscured by the wrong CPU model. With EB timers active, the firmware emits
a dense unsigned-PCM-shaped byte stream on port `0x00`; this is now the primary
audio-output evidence. The real INT0 source/rate and
acknowledgment must now be recovered from the CPU subtype and `0x9900` access
family. The primary Intel manual proves the firmware relocates an 80C188EB
peripheral control block to physical `0x9500`; the current base-80186 model
therefore leaves the firmware's EOI and interrupt-control writes as ordinary
RAM. Runtime segment capture proves the sample boundary
is physical `0x09900`; the ready branch reads an 8-bit byte from a 4 KiB ring
addressed by `SS:SI` and writes it to that register. The compatible RC Systems
hardware specifies an 8-bit, 4-11 kHz PCM path, but the exact ISA register
semantics and fixed text-to-speech rate remain unproven.

## Open Questions

- What real board event asserts INT0, and what acknowledges or gates the next
  event after the firmware services it?
- What exact modes and gates does external port `0x80` control?
- Where are the audible phoneme sample regions, and what exact timer-derived
  sample rate and byte encoding does the firmware use?
- Does the firmware write samples directly to an external DAC port or configure
  DMA/timing hardware to consume memory autonomously?

## Next Action

Commit and push the verified direct-DAC slice. Then update the runnable
documentation with the exact minimal-slot command, WAV recording and card-only
channel extraction, verify those commands from a clean tree, and preserve the
verified `HELLO` artifact locally.

## Reproduction command

```sh
./doubletalk pcv20 -bios glabios_0.24 -isa1 "" -isa4 "" -kbd "" \
    -isa6 doubletalkpc -rompath roms -nothrottle -video none -sound none \
    -autoboot_script scripts/doubletalk_trace_phrase.lua
```

The command exits successfully after printing one line prefixed with
`DOUBLETALK_RESULT`. With the synthetic EOI removed, the current kept result is
`bytes_sent:6`, `bad_transfer:false`, `cs:"0x8000"`, intact vector
`2a1e0080`, and converged read/write pointers `0x14a6/0x14a6`.
