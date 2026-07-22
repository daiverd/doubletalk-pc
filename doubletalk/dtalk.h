/* license:BSD-3-Clause
 *
 * dtalk.h - C API for the standalone RC Systems DoubleTalk PC emulator.
 *
 * Designed for both batch use (rusty_tts provider: queue text, drain PCM)
 * and streaming screen-reader use (NVDA synth driver: incremental feed,
 * immediate stop, index markers, pull PCM as it is generated).
 *
 * Audio format: unsigned 8-bit mono PCM at dtalk_sample_rate() Hz (10504,
 * the card's own 952-CPU-cycle timer cadence).
 */

#ifndef DTALK_H
#define DTALK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(DTALK_DLL)
#  ifdef DTALK_BUILD
#    define DTALK_API __declspec(dllexport)
#  else
#    define DTALK_API __declspec(dllimport)
#  endif
#else
#  define DTALK_API
#endif

typedef struct dtalk dtalk;

/* Create an instance from the 512KB firmware ROM image (doubletalkpc.bin)
 * and boot it to ready (~0.1s emulated, a few ms wall). NULL on failure. */
DTALK_API dtalk *dtalk_create(const void *rom, size_t rom_size);
DTALK_API void dtalk_destroy(dtalk *dt);

/* Hard reset (power cycle) and reboot to ready. Drops all queued input,
 * pending audio, and index marks; resets the output sample counter. */
DTALK_API void dtalk_reset(dtalk *dt);

DTALK_API uint32_t dtalk_sample_rate(const dtalk *dt);

/* Speech-rate boost, for screen-reader users who want speech faster than the
 * firmware's documented maximum (nS 9). The command interface caps nS at 0-9
 * (input above 9 wraps mod 10, per the chip family's default "parameter wrap"
 * behaviour), so a real >9S cannot be sent. Instead this rescales the ROM
 * speech-rate period table in our private in-RAM firmware copy (never the
 * on-disk ROM): smaller periods = proportionally faster speech at every nS
 * value, with pitch unchanged (the table drives frame duration, not F0) and
 * without touching the parser.
 *
 * level 0 = authentic (stock table, the default). levels 1..dtalk_rate_boost_
 * max() are progressively faster, each a verified-safe operating point (pitch
 * stable, speech intelligible, no firmware fault at any nS 0-9). Out-of-range
 * levels are clamped. The setting persists across dtalk_reset() and applies to
 * the next utterance. Under boost the host still sends ordinary nS 0-9; only
 * the nS -> real-duration mapping gets faster. */
DTALK_API int  dtalk_rate_boost_max(void);
DTALK_API void dtalk_set_rate_boost(dtalk *dt, int level);
DTALK_API int  dtalk_get_rate_boost(const dtalk *dt);

/* Read the card's host-visible LPC status/data port (ISA base+0). Returns
 * 0x7F when idle. A host-level ISA-port shim can present this alongside the
 * TTS status byte so DOS/Linux screen-reader drivers probe the card the way
 * they do on real hardware: Linux dtlk.c reads a 16-bit word at the base
 * port and requires (word & 0xfbff) == 0x107f - i.e. this LPC byte == 0x7F
 * and the TTS status byte's RDY bit (0x10) set - before treating the card as
 * present. Values other than 0x7F carry index-marker bytes (dtlk_read_lpc). */
DTALK_API uint8_t dtalk_lpc_status(dtalk *dt);

/* Queue raw bytes for the card's TTS input port: printable text plus any
 * embedded control codes from the manual (0x01-prefixed commands; CR or
 * NUL starts speech in the default text mode). Bytes are delivered to the
 * card RDY-gated as it accepts them while dtalk_synth() runs. */
DTALK_API void dtalk_queue(dtalk *dt, const void *bytes, size_t len);

/* Convenience: queue text followed by CR. */
DTALK_API void dtalk_say(dtalk *dt, const char *text);

/* Immediate stop (Ctrl-X / DTLK_CLEAR, written un-gated per the manual):
 * drops the host-side queue, stops speech, flushes the card's buffer, and
 * discards pending audio and index marks. Synthesizer settings persist. */
DTALK_API void dtalk_stop(dtalk *dt);

/* Nonzero while speech is in progress or input is queued/buffered. */
DTALK_API int dtalk_active(dtalk *dt);

/* Run the emulation forward, producing up to max_samples of PCM into out.
 * Returns the number of samples produced; 0 only once idle (speech done,
 * nothing queued). Call repeatedly to stream. */
DTALK_API size_t dtalk_synth(dtalk *dt, uint8_t *out, size_t max_samples);

/* Same semantics as dtalk_synth, but returns signed 16-bit samples run
 * through a modeled output stage that approximates what the MAME ISA-card
 * emulation does to the identical raw DAC bytes (the cleaner reference the
 * user A/Bs against): a DC-blocking high-pass to kill utterance-boundary
 * steps, a 2-pole reconstruction low-pass that smooths the 10.5kHz zero-
 * order-hold staircase (the source of the audible hiss), and MAME's 0.5
 * output-route headroom gain so loud presets (e.g. Big Bob) come off the
 * rails. Runs at the same dtalk_sample_rate(); filter state lives in the
 * instance and is reset by dtalk_reset()/dtalk_stop(). The plain 8-bit
 * dtalk_synth() path is byte-for-byte unchanged. */
DTALK_API size_t dtalk_synth16(dtalk *dt, int16_t *out, size_t max_samples);

/* Set the reconstruction low-pass corner (Hz) used by the dtalk_synth16
 * output stage, recomputing the Butterworth biquad coefficients in place.
 * Everything else in the output stage (the DC-blocking high-pass, the 0.5
 * headroom gain, the Direct-Form-I biquad topology) is unchanged; only the
 * low-pass corner moves.
 *
 * The default is 3000 Hz - the RC8650/RC8660 datasheets' own recommended
 * "3 kHz Low-Pass Filter" output stage, i.e. the authentic card sound, and
 * the value MAME's resampler spectrally matches. Passing 0 restores that
 * default. Higher corners (up to a sensible fraction of the ~10.5kHz Nyquist
 * headroom) trade that authenticity for brightness by letting more of the
 * DAC's high-frequency content through; the argument is clamped to the
 * range 500..5000 Hz.
 *
 * Safe to call mid-stream: the filter's sample history is preserved, only
 * the coefficients are swapped, so changing the setting while speaking does
 * not introduce a discontinuity. Coefficients (and the chosen corner) persist
 * across dtalk_stop()/dtalk_reset(); those only clear the filter's history. */
DTALK_API void dtalk_set_lowpass_hz(dtalk *dt, uint32_t hz);

/* Index markers (embedded Ctrl-A <n> I, n = 0-99): each marker reached by
 * the speech output is queued with the absolute output-sample position at
 * which it fired (positions count all samples produced since create/reset,
 * i.e. cumulative dtalk_synth() output). */
typedef struct {
	uint8_t value;
	uint64_t sample_pos;
} dtalk_index_mark;

/* Dequeue up to max reached markers; returns how many were written. */
DTALK_API size_t dtalk_read_index_marks(dtalk *dt, dtalk_index_mark *out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* DTALK_H */
