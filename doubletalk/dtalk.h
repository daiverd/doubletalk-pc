/* license:BSD-3-Clause
 * copyright-holders:David Sexton
 *
 * dtalk.h - C API for the standalone RC Systems DoubleTalk PC emulator.
 *
 * Designed for both batch use (a host provider queues text, drains PCM)
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

/* Convenience: queue text followed by CR. The pronunciation dictionary, if
 * one is set, is applied first. */
DTALK_API void dtalk_say(dtalk *dt, const char *text);

/* --- pronunciation dictionary -------------------------------------------- */

/* Substitute pronunciations into text on its way to the card: respellings,
 * phonemes through the card's own phoneme mode (Ctrl-A D), or embedded
 * commands. The rules, the file format and the loaders belong to rcdict, which
 * is mirrored verbatim from upstream -- include rcdict/rcdict.h to build one.
 * All of it is optional and off until asked for.
 *
 * The dictionary is BORROWED: it must outlive the instance, or be replaced
 * with NULL before it is freed. inline_phonemes switches on the "[[K AE T]]"
 * escape in ordinary text, which is off by default because "[[" is wiki link
 * syntax and a user reading a wiki must not lose text to it. */
struct rcdict;
DTALK_API void dtalk_set_dictionary(dtalk *dt, const struct rcdict *d,
                                    int inline_phonemes);

/* Run the dictionary over text WITHOUT queueing it, returning the number of
 * bytes the result needs (not counting a terminating NUL); a value >= outcap
 * means it was truncated and the call should be repeated bigger.
 *
 * For callers that split long text into utterances themselves: one word can
 * expand into forty characters of phonemes plus the mode switches, so
 * splitting first and expanding after can push a piece past what the card will
 * take. Expand, then split. */
DTALK_API size_t dtalk_expand(dtalk *dt, const char *in, size_t inlen,
                              char *out, size_t outcap);

/* Building a dictionary, for callers that cannot reach rcdict's own symbols.
 *
 * A C caller that links libdtalk.a should just include rcdict/rcdict.h and use
 * rcdict_new/rcdict_add_file directly -- these add nothing. They exist for the
 * DLL: dtalk.h marks its entry points __declspec(dllexport), which turns off
 * mingw's export-everything default, so rcdict's own symbols are not in the
 * DLL's export table and a ctypes caller cannot see them. Re-exporting the
 * four calls a host actually needs is a great deal less crude than exporting
 * every symbol in the image, and it keeps the export marker out of rcdict --
 * which has to stay free of anything Windows- or project-specific if it is to
 * go on being shared verbatim with its other host.
 *
 * dtalk_dict_new also settles the profile (rcdict_doubletalk_pc) here rather
 * than making the caller name it. That is the one piece of the construction a
 * host has no business choosing.
 *
 * The result is BORROWED by dtalk_set_dictionary: detach it with NULL before
 * dtalk_dict_free. */
DTALK_API struct rcdict *dtalk_dict_new(void);
DTALK_API void dtalk_dict_free(struct rcdict *d);

/* Append one file's rules, returning how many were added (0 on any failure:
 * unreadable file, or a file whose every line was rejected). Rules are
 * first-match-wins in load order across all files, so a later file can only
 * add -- to override an earlier one it has to be loaded first. */
DTALK_API int dtalk_dict_add_file(struct rcdict *d, const char *path);

/* Total rules held, across every file added. */
DTALK_API size_t dtalk_dict_rule_count(const struct rcdict *d);

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
 * The default is 3800 Hz, the corner that best matches the MAME reference this
 * output stage is modeled on, now that the sample-rate conversion feeding it is
 * band-limited and no longer eats the highs itself (see LPF_HZ in dtalk.cpp for
 * the measurements). 3000 Hz remains available and is the RC8650/RC8660
 * datasheets' own recommended "3 kHz Low-Pass Filter" output stage; it is the
 * authentic analog corner but sounds darker than the reference. Passing 0
 * restores the default.
 *
 * Lower corners muffle; higher ones brighten by letting more of the DAC's
 * high-frequency content through. The argument is clamped to 500..5000 Hz.
 * The upper bound is not a matter of taste: the output stage runs at
 * ~10504 Hz, so above its 5252 Hz Nyquist the biquad's bilinear prewarp folds
 * and the filter becomes unstable. Values such as 8000 are not meaningful
 * here, and 5000 is already close to no filtering at all.
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
