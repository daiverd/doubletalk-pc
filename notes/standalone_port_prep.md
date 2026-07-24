# Standalone Port Prep — Learnings from the MAME-based Provider

Before writing a from-scratch (non-MAME) DoubleTalk implementation (the CPU-port
plan lives with the parent TTS project), this collects what building and tuning
the MAME-based provider taught us, plus what the real RC Systems DoubleTalk PC/LT
User's Manual confirms about the official protocol - some of which we only knew
from reverse-engineering before.

## The big architectural lesson: don't couple emulated time to wall-clock time

**Increasing the emulated CPU clock increases speech speed *and* pitch by
the same factor.** Verified directly: the card's DAC is written straight
from CPU-timer-driven firmware code (`doubletalkpc.cpp`'s `cpu_io()`, port
0x00), with no separate audio clock domain. Bumping the driver's
`I80C188EB` clock (`device_add_mconfig`) from stock `20_MHz_XTAL` (10MHz
processor clock) by +10% sped speech up ~10% and raised pitch to match
(genuine "chipmunk" effect - same as a tape recorded too fast); doubling it
to 40MHz halved real synthesis time and doubled pitch. In the MAME path we
compensated for the 2x case by halving the *declared* output sample rate at
encode time (not resampling - literally just the WAV/MP3 header value),
which exactly and losslessly undid both the pitch rise and the tempo
speedup, because MAME's audio mixer stream rate (48kHz) is fixed
independent of the card's own CPU clock.

This matters for the standalone port because **we won't need that
workaround at all** if we design it correctly from the start. MAME's
`-nothrottle` already demonstrates the right model: emulated time and
wall-clock time are fully decoupled. An emulated CPU/timer doesn't
perceive real time passing - it only perceives instruction counts and
internal timer ticks. If the standalone core free-runs its interpreter (no
artificial pacing to match real 10MHz timing) and generates DAC samples
purely from that internal instruction-count-driven state machine, the
*output* correctly encodes normal-speed, normal-pitch audio - it just gets
computed as fast as the host CPU can interpret instructions, which for a
16-bit 10MHz-class instruction set should be extremely fast on any modern
host. This gets us the equivalent of MAME's ~13-14x nothrottle speedup (or
better, since we won't be paying for an idle host-PC CPU alongside it - see
below) with **zero** audio quality trade-off, instead of needing an
overclock-plus-compensating-sample-rate hack.

Corollary finding from the same investigation: profiling showed the
host-PC side of the MAME machine (GLaBIOS boot + idle V20 CPU) was costing
~25-30% of total interpreter cycles even though it does nothing useful for
us after boot (just shuttles bytes to one I/O port). The standalone port
sidesteps this issue entirely by construction - there's no "host PC" to
emulate at all, just the DoubleTalk card's own 80C188EB core plus a trivial
byte-in/audio-out interface.

## Real protocol, confirmed from the official manual

We'd reverse-engineered the RDY-gated host I/O protocol and the `<Ctrl+A>
<digit>s` speed command from ROM/driver tracing before finding this repo
already had the RC Systems DoubleTalk PC/LT User's Manual to hand. It confirms
everything we found and fills in a lot more:

- **Command format**: `<command character><param><letter>`. Default
  command character is **Control-A (0x01)**, matches what the parent TTS project's
  backtick shortcut sends. It's *changeable* to any control character
  0x01-0x1A by sending `<current><new>`, and resettable unconditionally to
  Control-A via Control-^ (0x1E) at any time. To speak the command
  character itself, send it twice in a row.
- **Param is 1 or 2 ASCII digits**, either absolute (`9S`) or *relative*
  (`+2V`/`-2V`, adjusts from current value, **wraps** rather than clamps at
  the range boundary) - we never tested relative params.
- **Full command set** (we'd only found/used Speed):
  | Command | Letter | Range | Default | Notes |
  |---|---|---|---|---|
  | Voice | `nO` | 0-7 (8 voices) | - | Perfect Paul, Vader, Big Bob, Precise Pete, Ricochet, Biff, Skip, Robo Robert |
  | Articulation | `nA` | 0-9 | 5 | should scale up with Speed per the manual |
  | Expression | `nE` / `E` | 0-9 | 5 | intonation/pitch variation; `E` alone re-enables at last value |
  | Monotone | `M` | - | - | equivalent to `0E` |
  | Formant Frequency | `nF` | 0-9 | 5 | vocal tract formant shift |
  | **Speed** | `nS` | **0-9** | **5** | matches our finding exactly - single-digit range is a real firmware-level cap, not a parser limitation (see Pitch below) |
  | Pitch | `nP` | **0-99** | 50 | two-digit param - proves the parser *can* read 2 digits; Speed's 0-9 ceiling is a genuine hardware/firmware design limit, not something we could've unlocked by sending more digits |
  | Volume | `nV` | 0-9 | 5 | also affects PCM/tone-generator output |
  | Tone | `nX` | 0-2 | 1 | bass/normal/treble |
  | Reverb | `nR` | 0-9 | 0 | |
  | Text Mode/Delay | `T`/`nT` | - | - | |
  | Character Mode/Delay | `C`/`nC` | - | - | |
  | Phoneme mode | `D` | - | - | |
  | PCM mode | `#`/`n#` | 0-99 | - | non-buffered vs. buffered |
- **Operating modes** (case-insensitive text/commands throughout):
  - **Text mode** (default): won't speak until CR (0x0D) or Null (0x00) -
    matches what we implemented.
  - **Character mode**: spells character-by-character, no CR/Null wait.
  - **Phoneme mode**: raw phoneme access, also waits for CR/Null.

## Hardware specs, confirmed

From the manual's spec sheet (matches everything we found independently):

- **CPU**: Intel 10MHz 80C188EB - official confirmation of the CPU-model
  fix (generic 80186 was wrong; needed the 80C188EB's relocatable
  Peripheral Control Block) that got real audio working in the first
  place.
- **Memory**: 512K ROM + 8K RAM - matches our ROM dump size exactly.
- **TTS synthesizer**: **3K input buffer**. The parent TTS project's MAME provider caps
  input at 200 characters (`_mame_audio.py`'s `_MAX_TEXT_LEN`) - that's an
  arbitrary safety margin we chose, *not* a real hardware limit. Worth
  reconsidering for the standalone port, which could reasonably support up
  to ~3KB of input per request.
- **LPC synthesizer**: 4K data buffer, TMS5220-compatible **and** "D6"
  data formats, 8kHz sample rate, 2 speeds.
- **PCM synthesizer**: 8-bit mono PCM/ADPCM, 4K sample buffer, 100
  selectable rates from 4-11kHz, or 0-48kHz in non-buffered mode.
- **CVSD**: 32kbps.
- **Audio out** (PC card): 0.5W into 8 ohms, DC-coupled bridge-tied output,
  mono 3.5mm jack - consistent with what we found empirically (unsigned
  8-bit DAC, silence-center ~0x80).
- **I/O**: no IRQ, no DMA, no system memory requirement - purely two
  polled 8-bit I/O ports, jumper-selectable among six fixed base addresses
  (25E/F, 29E/F, 2DE/F, 31E/F, 35E/F, 39E/F). Matches the Linux `dtlk.c`
  driver's port list and our hardcoded 0x25E/0x25F choice exactly.

## Resolved: the startup DAC click is genuine hardware behavior

Every single MAME capture shows an identical, deterministic ~100ms
full-scale transient on the DAC channel at t=0, before any text is even
sent - same exact sample values every run, clearly not related to content.
We work around it in `providers/doubletalk.py` by skipping the first 150ms
before silence-trimming. This was flagged as an open question (genuine
hardware power-on click vs. a MAME-emulation-specific artifact) but is
confirmed to be real hardware behavior - the real DoubleTalk card does
this too on power-on, consistent with the "DC-coupled bridge-tied" output
spec above (a real power-on transient through a DC-coupled amp is exactly
what you'd expect).

Implication for the standalone port: this should be *reproduced*, not
treated as a bug to eliminate - if the from-scratch CPU/DAC reset path
naturally produces an equivalent click on cold start, that's a sign the
init sequence is faithful to the real chip, not something to suppress.
Downstream consumers (e.g. a provider built on the standalone core) will still
want the same kind of leading-click trim the parent TTS project's MAME provider
already does, since it's not meaningful speech content either way.

## What we did *not* re-derive here (already documented elsewhere)

The detailed memory map and hardware register findings (ROM base
0x80000-0xFFFFF, host mailbox at physical 0xA100 + INT1 doorbell,
Peripheral Control Block relocation via RELREG at I/O port 0xFFA8 to
0x9500, CPU-side port 0x00 = DAC byte / port 0x40 = TTS status byte with
its SYNC/SYNC2/RDY/AF/AE bit layout) come from our own ROM disassembly and
MAME driver work, not the manual - see `notes/investigation-audio-path.md` and
`driver/` in this repo. Not duplicated here.

## Recommendations for the standalone port

1. Free-run the CPU interpreter (no real-time pacing) and drive DAC
   sample generation purely off internal instruction/timer state, the way
   MAME's `-nothrottle` does - gets the speed win with no pitch/tempo
   trade-off, no compensating-sample-rate hack needed.
2. Support the full command set (Speed/Pitch/Volume/Voice/Articulation/
   Expression/Formant/Tone/Reverb), not just Speed - now that the wire
   format's confirmed, it's cheap to add all of them together, and it's a
   real feature surface for the parent TTS project callers beyond what the MAME-based
   provider currently exposes.
3. Reconsider the 200-char input cap given the real 3K buffer.
4. Once the port has independent reset/init behavior, check whether the
   ~100ms startup click reproduces - resolves the open question above.
