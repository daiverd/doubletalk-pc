<!--
license:BSD-3-Clause
copyright-holders:David Sexton
-->

# Audio resampling: the DAC clock, `pull_samples()`, and the output filter

An investigation into whether `doubletalk_board::pull_samples()` was
reconstructing the card's PCM stream correctly. It started from a proposed
rewrite of that function, and ended up changing the function, the output
filter's calibration, and this project's understanding of what the `nF`
formant command actually does.

Everything below was measured against the firmware running in this emulator,
with the companion MAME DoubleTalk PC driver as the reference for how the
finished audio should sound.

## Assumptions going in

These were the beliefs the code was written under. Two of the three turned out
to be wrong.

1. **The firmware's timer ISR fires every 952 CPU cycles**, giving a fixed
   10504 Hz DAC rate. (`dtalk.cpp` said "one DAC write per 952 CPU cycles
   (measured)"; `notes/PORTING.md` derives 952 from `10,000,000 / 10,500`.)
2. **That rate is constant**, so mapping DAC writes onto a fixed 10504 Hz
   output grid is a formality.
3. **A zero-order hold is the faithful model**, because the card's R-2R DAC
   physically holds its value between writes.

Assumption 3 holds. Assumptions 1 and 2 do not.

## Claims proved

- **`m_dac_events` is a queue of PCM samples, not level-change events.** Every
  `io_write` to CPU port 0x00 pushes exactly one sample.

- **The old `while (front().first <= sample_cycle)` loop discarded input.** It
  popped every event at or before each output instant and emitted only the
  last. Measured discard rate against the requested rate:

      10504 Hz (native)          0.57 %
      11025 / 22050 / 44100 Hz   0 %
      8000 Hz                    23.8 %

- **The ISR period is not constant.** Over 19,990 intervals of real speech at
  the default formant setting: min 907, max 998, mean 951.999, std 5.35
  cycles, 43 distinct values, with the modal 952 accounting for only 15 % of
  them. The spread is the firmware's own interrupt latency. It is jitter, not
  drift, so the long-run rate is stable.

- **The old grid slipped against that jitter.** The grid steps by
  10,000,000 / 10504 = 952.02 cycles while writes land ~952 apart. Aligning the
  old output against the exact DAC event sequence over 7.66 s of speech:

      repeated samples:  125  (16.3/s)
      dropped samples:   127  (16.6/s)
      total slips: 252 = 0.31 % of samples, 32.9 per second

  Each slip drops one written sample and repeats a neighbour.

- **Resampling to a materially lower rate aliases.** The grid path does no
  band-limiting, so it decimates without an anti-alias filter. Linear
  interpolation does not fix this - against a reference containing content
  above the 8 kHz Nyquist, zero-order hold scored 3.4 dB and linear
  interpolation 10.8 dB. Better, still wrong.

## Claims disproved

- **"The output rate is fixed."** It is not. See the `nF` finding below - this
  is the important one.

- **"The scratchiness comes from the host requesting a different
  `sample_rate_hz`."** Nothing requests a different rate. All three call sites
  in `dtalk.cpp` pass `SAMPLE_RATE`, and the NVDA add-on hands 10504 Hz
  straight to `nvwave.WavePlayer` for NVDA's audio stack to resample. The
  conclusion that real resampling was needed turned out to be right, but for a
  reason on the *source* side, not the destination side.

- **"The grid mapping is unnecessary at the native rate, so DAC events can be
  emitted 1:1."** This was tried. It made the `nF` formant control a complete
  no-op: all ten settings produced byte-identical output of identical duration.
  The approach was abandoned. The grid is load-bearing.

- **"3 kHz is both the datasheet's filter and the MAME-matching value."** It
  was, under a zero-order-hold stream. It is not under an interpolated one.

## The `nF` finding: formant frequency is a sample-clock control

Sweeping every speech command and measuring the port-0x00 cadence on the same
phrase:

    cmd            events   mean delta   DAC rate       sample values
    A 2/7    18364/21752      951.999    10504.21 Hz    change
    E 2/7    20669/20036      952.000    10504.20 Hz    change
    P 23/77  20120/19201      951.999    10504.21 Hz    change
    R 2/7    19991/19991      952.000    10504.21 Hz    change
    S 2/7    26049/17695      952.000    10504.21 Hz    change
    V 2/7    19991/19991      951.999    10504.21 Hz    change
    X 0/2    19991/19991      952.000    10504.20 Hz    change
    F 0            19991     1011.856     9882.8 Hz     IDENTICAL
    F 2            19991      987.916    10122.3 Hz     IDENTICAL
    F 5            19991      952.006    10504.1 Hz     IDENTICAL
    F 7            19991      928.066    10775.1 Hz     IDENTICAL
    F 9            19991      904.126    11060.4 Hz     IDENTICAL

`nF` is the only command that retunes the DAC clock, and the only one that
leaves the emitted sample values byte-identical - the checksum of the value
stream is the same for all ten settings, and equal to the no-command case. The
timer reload is `952 - 12*(F-5)`.

So formant frequency on this card is implemented purely as **varispeed**: the
same samples played at a different clock, which shifts formants, pitch and
duration together. The board's output rate genuinely varies over
9883..11060 Hz, and `pull_samples()` must do a real rate conversion. Converting
that variable source rate to a fixed output rate is precisely what makes `nF`
audible.

## What we did

**`pull_samples()` is now an interpolating resampler.** Each output instant is
linearly interpolated between the DAC writes bracketing it. The bracket lives
in members so it survives across calls - the caller pulls every
`RUN_CHUNK_CYCLES`, so most instants find nothing new to pop. When the bracket
spans more than two DAC periods the value is held instead of ramped: the timer
ISR stops between utterances, and across that silence the real DAC holds rather
than sliding toward the next utterance's first sample.

Two-point interpolation does not band-limit, so it is only honest for output
rates near the DAC clock - which covers the whole formant range. A materially
lower requested rate still needs an anti-alias low-pass; that is documented at
the function rather than implemented, since nothing asks for one.

**The output stage's low-pass default moved from 3000 Hz to 3800 Hz.** The
3000 Hz corner was calibrated against the MAME reference when `pull_samples()`
emitted a zero-order-hold staircase, and it also matched the RC8650/8660
datasheets' recommended "Low Cost 3 kHz Low-Pass Filter" - the two references
agreed, which read as confirmation. Interpolation breaks that agreement by
removing about half the staircase's high-frequency energy at the source (raw
energy above 3 kHz: 0.0287 held, 0.0142 interpolated), so at 3000 Hz the
reconstruction filtering happens twice and the voice comes out darker than the
card.

Re-measured against a MAME capture of the same phrase - note the card's audio
is on the capture's **right** channel, the left being the PC speaker, and
mixing them contaminates the result. Spectral tilt of the 2-5 kHz band against
0.3-2 kHz, 215 speech frames:

    MAME reference           -7.57 dB   (target)
    old shipped, ZOH @ 3000  -8.87 dB
    interpolated @ 2000     -14.60 dB
    interpolated @ 3000     -10.32 dB   (double-filtered)
    interpolated @ 3800      -8.91 dB   (-0.04 vs old shipped)
    interpolated @ 4800      -8.41 dB   (-0.84 vs MAME, closest)
    interpolated @ 5000      -8.40 dB

3800 Hz reproduces the voice this project has always shipped to within
0.04 dB, so the resampler fix lands with no tonal change. Worth recording
honestly: **no corner reaches the reference.** The model is ~1.3 dB darker than
MAME either way, and 3 kHz is no longer the MAME-matching value, so the
datasheet and the reference now genuinely disagree. That tension is documented
at `LPF_HZ` rather than papered over.

**The low-pass range is 500..5000 Hz, and cannot be raised.** The output stage
runs at 10504 Hz, so its Nyquist is 5252 Hz. `set_lowpass()` prewarps with
`tan(pi*hz/SAMPLE_RATE)`, which diverges at Nyquist and goes negative above it,
putting the biquad's poles outside the unit circle:

    corner   prewarp   poles inside unit circle
    3800        2.16   yes
    5000       13.24   yes
    5252       +inf    degenerate (all-pass)
    6000      -4.40    NO - unstable
    8000      -0.93    NO - unstable

A corner of 6000 or 8000 does not give a gentler filter, it gives a divergent
one - and at 8000 the magnitude response still *looks* plausible, which is the
trap. 5000 Hz is the practical ceiling and is already close to no filtering
(|H| at 5 kHz is 0.707); the raw stream carries only 0.0014 of its energy above
4 kHz, which is why 4800 and 5000 measure identically. A `static_assert` pins
`LPF_HZ_MAX` below Nyquist so this cannot regress silently.

**The NVDA add-on's Filter setting grew from two entries to five:**

    2000  Muffled (2 kHz)
    3000  Classic (3 kHz)     - the datasheet corner
    3800  Default (3.8 kHz)   - new default
    4800  Wide (4.8 kHz)
    5000  Widest (5 kHz)      - ceiling

Existing saved configurations holding `3000` or `4800` remain valid and keep
working.

## Verification

- Formant still behaves, and identically to the old code: 8.403 / 7.923 /
  7.533 s at 0F / 5F / 9F, unchanged across the rewrite.
- `dtalk_cli stoptest` reports 0-sample index-mark drift after 30 cancels.
- `dtalk_cli booststress` passes; `boot` and `dacprobe` unchanged.
- `dtalk_set_lowpass_hz(8000)` clamps to 5000, byte-identical to an explicit
  5000.
- Both mingw DLL targets build.

## A note on the proposed rewrite that started this

The patch that prompted the investigation replaced the hold-and-pop loop with
linear interpolation. Its direction was right and its reasoning was not, but
more usefully, it carried three defects worth recording as things to avoid:

1. **An early `return` when the event queue is empty**, which skipped
   `m_audio_emitted_cycles = now` and leaked emulated time. Trailing audio was
   silently dropped.
2. **Interpolation state held in locals.** With nothing carrying across calls,
   a silence gap left the bracket unseeded, and the code seeded it from a
   *future* event - filling the entire gap with the next utterance's level.
   Replaying a 200 ms gap between an utterance ending at 128 and one starting
   at 200 gave `128 x 65` then `200 x 2100`, where the correct output is
   `128 x 2152` then `200 x 895`. A DC block at the wrong amplitude, a hard
   step into it, and a click through the downstream DC blocker, on every
   inter-utterance gap.
3. **No guard against interpolating across that gap**, which is the underlying
   modelling error: the DAC holds there, it does not ramp.

The current implementation addresses all three - persistent bracket state, no
early return, and the two-period span limit.

---

# Round two: linear interpolation was not band-limited

Two later user reports turned out to be the same defect in the interpolating
resampler this document describes.

**Report A.** Reading the same word repeatedly in NVDA, with a pause between
each, the level of the `/s/` audibly shifted from one repetition to the next.
Not reproducible in MAME.

**Report B.** Even with the filter set to 5 kHz, MAME sounded brighter - "more
highs" - than the emulator.

## Both are the sinc^2 response of two-point interpolation

Linear interpolation reconstructs with a triangular kernel, whose frequency
response is `sinc^2`. At 5 kHz against a 10504 Hz source that is **-7.0 dB**,
where a zero-order hold (response `sinc`) costs only **-3.5 dB**. That is
report B directly: the loss is in the resampler, so no low-pass corner can
recover it - raising the corner to 5000 does nothing about content the
resampler already discarded.

Report A is the same loss made time-varying. How much a `sinc^2` kernel
attenuates depends on *where in the source period the output instant lands* -
full gain when it lands on a DAC write, maximum loss when it lands halfway
between. The output grid is 10504 Hz (integer) while the true DAC rate is
`CPU_HZ/952` = 10504.20 Hz, so that phase slides continuously, and consecutive
utterances begin at unrelated phases.

Measured, 40 repetitions of "SPACE" in one session:

| | RMS spread across repetitions |
|---|---|
| linear interpolation | **8.96%** (0.74 dB), oscillating 772 -> 841 -> 772 |
| band-limited kernel | **0.28%** (0.024 dB) |

The oscillation is bounded and cyclic - it returns to where it started, which
is what identifies it as a phase beat rather than an accumulating leak.
Utterances rendered in *separate* sessions were always bit-identical, because
each session starts at phase zero; only within a session does the phase walk.

## The fix

`pull_samples()` now interpolates with a windowed sinc (Lanczos, 8 taps each
side) on the **source sample index** axis: DAC write *k* is sample *k* of a
sequence clocked at the firmware's ISR rate, each output instant is located as
a fractional index between its bracketing writes, and the kernel is evaluated
there. When the source runs faster than the grid (7F/9F) the kernel cutoff
scales to the output Nyquist so decimation stays anti-aliased. Weights are
normalized per sample, since a windowed sinc's taps do not sum to exactly 1 at
an arbitrary fractional offset and the residual would be broadband gain ripple.

Gain is now flat to the passband edge regardless of phase, which fixes both
reports at once. On an identical phrase, raw pre-filter energy above 3 kHz:

| | energy > 3 kHz |
|---|---|
| linear interpolation | 0.0216 |
| band-limited kernel | 0.0468 |

A **2.17x** recovery - mirroring the 2.02x that interpolation was destroying
(0.0287 -> 0.0142 in the original measurement above).

Two implementation details the design forces:

* **Lookahead.** The kernel reaches 8 samples right of the output instant, so
  output lags `now` by 8 source periods and events are retired only once they
  fall off the *left* edge of the support - `m_dac_events` is no longer drained
  down to the current instant. `pull_samples(..., drain=true)` flushes the tail
  (clamping missing right taps to the last DAC level, which is what the DAC
  holds anyway); boot and cancel pass it so nothing is left pending to surface
  inside a later utterance. End of utterance needs no special case: the 150 ms
  idle settle advances `now` well past the 0.82 ms margin.
* **The gap guard survives.** A bracket wider than two DAC periods is still the
  silence between utterances, not a sample interval, and is still held rather
  than reconstructed - band-limited reconstruction across it would ring.

## Consequence for `LPF_HZ`

3800 stays, but its justification inverts. It was chosen to *cancel* the
interpolator's HF loss; with that loss gone it is simply the corner that best
matches the MAME reference - and it now reaches it, where previously "no corner
reaches MAME's -7.57; the model is ~1.3 dB darker either way". The datasheet's
authentic 3000 remains available and remains darker than the reference.

## Verification

* Formant varispeed preserved: 0F 11601 samples > 5F 11076 > 9F 10656.
* `stoptest`: index marks drift **0 samples** after 30 cancels.
* `booststress`: STRESS PASS, idle after every phrase.
* 21.7x realtime - the 16-tap kernel is negligible beside the 80C188EB core.
