// license:BSD-3-Clause
// copyright-holders:David Sexton
// C API implementation over doubletalk_board. See dtalk.h.

#define DTALK_BUILD 1
#include "dtalk.h"

#include "doubletalk_board.h"

#include <cmath>
#include <deque>

namespace {

// Firmware timer cadence: one DAC write per 952 CPU cycles (measured).
constexpr u32 SAMPLE_RATE = doubletalk_board::CPU_HZ / 952; // 10504

// ---- modeled output stage (dtalk_synth16) ----------------------------------
// Reproduces what the MAME ISA-card build does to the identical raw DAC bytes
// (the cleaner reference the user compares against), not real analog hardware:
//
//   * MAME routes its 8-bit R-2R DAC at 0.5 (doubletalkpc.cpp:
//     add_route(ALL_OUTPUTS, "mono", 0.5)); measured, this is exactly the raw
//     0.703 Big-Bob peak scaled to MAME's 0.352 peak, so 0.5 is used verbatim
//     as the headroom gain - it takes the loud presets off the rails without
//     making voice 0 quiet, and matches the reference level bit for bit.
//   * MAME's sound_stream resamples the 10504Hz zero-order-hold stream up to
//     the host rate through a band-limiting filter, which smooths the DAC
//     staircase (the source of the "hiss while speaking"). The 2-pole
//     Butterworth low-pass below plays that role inside the 10504Hz stream.
//   * MAME's stream is continuous - no per-utterance restarts - so it never
//     emits the DC-step click our per-request path can. The DC-blocking
//     high-pass removes the standing DC offset that causes those clicks.
constexpr double DT_PI = 3.14159265358979323846; // DT_PI is not portable (mingw -std=c++20)
constexpr double DC_BLOCK_HZ = 20.0;    // one-pole high-pass corner
constexpr double LPF_HZ = 3000.0;       // default reconstruction low-pass corner.
                                        // Chosen by measurement: matching the
                                        // MAME reference's spectral tilt (its
                                        // resampler drops the raw HF-energy
                                        // ratio from 0.127 to 0.096) needs a
                                        // ~2.8-3.0kHz corner. This coincides
                                        // with the RC8650/8660 datasheets' own
                                        // recommended output filter, captioned
                                        // "Low Cost 3 kHz Low-Pass Filter" - so
                                        // MAME and the real card's spec agree.
constexpr double HEADROOM_GAIN = 0.5;   // MAME's DAC output route
                                        // (add_route(ALL_OUTPUTS,"mono",0.5))
// User-selectable low-pass corner bounds (dtalk_set_lowpass_hz). 3000 is the
// authentic datasheet default; the NVDA driver's "Wide" preset uses 4800.
constexpr double LPF_HZ_MIN = 500.0;
constexpr double LPF_HZ_MAX = 5000.0;

// Firmware text-buffer read/write pointers in CPU RAM (same addresses the
// MAME capture.lua watched): equal <=> input buffer fully consumed.
constexpr u32 BUF_READ_PTR = 0x000f;
constexpr u32 BUF_WRITE_PTR = 0x0011;

// Idle must hold this long (emulated) before we call the stream finished -
// covers the firmware's own latency between consuming text and raising
// SYNC when audio starts.
constexpr s64 IDLE_SETTLE_CYCLES = doubletalk_board::CPU_HZ * 3 / 20; // 150ms

constexpr s64 RUN_CHUNK_CYCLES = doubletalk_board::CPU_HZ / 100; // 10ms

// ---- rate boost (dtalk_set_rate_boost) -------------------------------------
// The firmware's speech rate (nS 0-9) is driven by a table of per-frame
// "period" words in ROM. Empirically (see notes), the speed setting d selects
// entry (10 + d) of the word table at ROM offset RATE_TABLE_OFF; the value is
// the frame period - smaller = faster speech, and it does NOT change pitch
// (verified F0-stable). The firmware's own consumer clamps the table index to
// [0,22], and the table already extends past the speed-9 slot (idx 19) to
// even smaller periods (idx 20-22), so shorter periods are values the firmware
// is built to handle. The parser wraps any nS input > 9 back into 0-9 (a mod,
// documented as the RC8650 "SAT=0 wrap" behaviour), so >9S cannot be reached
// through the command interface - hence we rate-boost by rescaling the ROM
// period table's rate region in our private in-RAM ROM copy (never the file).
//
// A boost LEVEL scales the whole rate region (idx 10-22) by a fixed percent so
// every nS 0-9 the host may still send stays monotonic and intelligible - only
// the mapping from nS to real duration gets faster. Level 0 is the authentic
// table. The percents are the verified-safe operating points (see PORTING /
// rate-boost verification): pitch stays within a couple percent and speech
// stays intelligible at every speed 0-9 down to level 3; below ~65% the
// firmware's frame engine floors and speech breaks up, so we do not expose it.
constexpr u32 RATE_TABLE_OFF = 0x48da;   // ROM offset of the period word table
constexpr int RATE_REGION_LO = 10;       // first rate-region index (speed 0)
constexpr int RATE_REGION_HI = 22;       // last (speed-9 slot is 19; 20-22 = extension)
// level -> percent of stock period. Index by level (0..RATE_BOOST_MAX).
// 100 = authentic. 88/80 are verified above the firmware's frame-duration
// floor at every nS 0-9 (below ~76% the fastest speeds saturate/garble - see
// the rate-boost verification), so both keep duration strictly monotonic in
// speed and preserve pitch (measured F0-identical across levels).
constexpr int RATE_BOOST_PCT[] = { 100, 88, 80 };
constexpr int RATE_BOOST_MAX = 2;

// Modeled analog output stage state (dtalk_synth16 only). Direct-Form-I
// biquad low-pass in series after a one-pole DC-blocking high-pass.
struct output_stage
{
	// DC-block one-pole high-pass: y = x - x1 + R*y1
	double r = 0.0;
	double hp_x1 = 0.0, hp_y1 = 0.0;
	// Butterworth low-pass biquad (Direct Form I)
	double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	double lp_x1 = 0, lp_x2 = 0, lp_y1 = 0, lp_y2 = 0;
	bool prime = true; // next input primes the DC-block so resume has no step

	output_stage()
	{
		r = 1.0 - (2.0 * DT_PI * DC_BLOCK_HZ / double(SAMPLE_RATE));
		set_lowpass(LPF_HZ);
	}

	// Recompute the 2-pole Butterworth low-pass biquad (bilinear transform)
	// for a new corner frequency, leaving the filter's sample history (and the
	// DC-block state) untouched so the swap is glitch-free mid-stream.
	void set_lowpass(double hz)
	{
		if (hz < LPF_HZ_MIN) hz = LPF_HZ_MIN;
		else if (hz > LPF_HZ_MAX) hz = LPF_HZ_MAX;
		const double w = std::tan(DT_PI * hz / double(SAMPLE_RATE));
		const double k = w * w;
		const double q = std::sqrt(2.0); // Butterworth: 1/Q = sqrt(2)
		const double norm = 1.0 / (1.0 + q * w + k);
		b0 = k * norm;
		b1 = 2.0 * b0;
		b2 = b0;
		a1 = 2.0 * (k - 1.0) * norm;
		a2 = (1.0 - q * w + k) * norm;
	}

	void reset(double level = 0.0)
	{
		hp_x1 = level;
		hp_y1 = 0.0;
		lp_x1 = lp_x2 = lp_y1 = lp_y2 = 0.0;
		prime = true;
	}

	int16_t process(double x)
	{
		if (prime)
		{
			// Seed the high-pass with this level so the first sample yields no
			// step (avoids a re-click when speech resumes after a stop/boot).
			hp_x1 = x;
			hp_y1 = 0.0;
			prime = false;
		}
		// DC-blocking high-pass
		double hp = x - hp_x1 + r * hp_y1;
		hp_x1 = x;
		hp_y1 = hp;
		// Butterworth low-pass biquad
		double lp = b0 * hp + b1 * lp_x1 + b2 * lp_x2 - a1 * lp_y1 - a2 * lp_y2;
		lp_x2 = lp_x1; lp_x1 = hp;
		lp_y2 = lp_y1; lp_y1 = lp;
		// headroom gain, clamp to int16
		double y = lp * HEADROOM_GAIN * 32768.0;
		if (y > 32767.0) y = 32767.0;
		else if (y < -32768.0) y = -32768.0;
		return int16_t(std::lround(y));
	}
};

} // namespace

struct dtalk
{
	doubletalk_board board;
	output_stage out16;                // modeled output stage for dtalk_synth16
	std::deque<u8> queue;              // host-side bytes not yet accepted by the card
	std::vector<u8> carry;             // pulled but not yet delivered samples
	size_t carry_off = 0;
	std::deque<dtalk_index_mark> marks;
	u64 samples_base = 0;              // board-grid samples discarded at boot
	u64 samples_dropped = 0;           // grid samples pulled/carried but never delivered (dtalk_stop)
	s64 idle_stable = 0;               // consecutive idle cycles observed
	int rate_boost = 0;                // current boost level (0 = authentic)
	u16 rate_orig[RATE_REGION_HI - RATE_REGION_LO + 1]; // pristine period words
	bool rate_saved = false;

	// Snapshot the stock ROM period table so boost levels can be re-derived
	// from the authentic values rather than compounding.
	void save_rate_table()
	{
		for (int i = RATE_REGION_LO; i <= RATE_REGION_HI; i++)
		{
			u32 off = RATE_TABLE_OFF + 2u * u32(i);
			rate_orig[i - RATE_REGION_LO] =
				u16(board.rom_peek(off)) | u16(board.rom_peek(off + 1)) << 8;
		}
		rate_saved = true;
	}

	// Rescale the in-RAM ROM period table's rate region for the given boost
	// level (always relative to the saved stock values).
	void apply_rate_boost(int level)
	{
		if (!rate_saved)
			save_rate_table();
		if (level < 0) level = 0;
		if (level > RATE_BOOST_MAX) level = RATE_BOOST_MAX;
		const int pct = RATE_BOOST_PCT[level];
		for (int i = RATE_REGION_LO; i <= RATE_REGION_HI; i++)
		{
			u32 w = u32(rate_orig[i - RATE_REGION_LO]) * u32(pct) / 100u;
			if (w < 1) w = 1;
			if (w > 0xffff) w = 0xffff;
			u32 off = RATE_TABLE_OFF + 2u * u32(i);
			board.rom_poke(off, u8(w & 0xff));
			board.rom_poke(off + 1, u8((w >> 8) & 0xff));
		}
		rate_boost = level;
	}

	bool card_busy() const
	{
		return (board.host_status() & 0x40) != 0 // SYNC: speech in progress
			|| board.ram16(BUF_READ_PTR) != board.ram16(BUF_WRITE_PTR);
	}

	void drop_pending_audio()
	{
		// These grid samples advanced the board's absolute output counter but
		// are thrown away instead of delivered by dtalk_synth. Track them so
		// index_events (timestamped in absolute grid samples) can be converted
		// back into delivered-stream positions - otherwise every mark after a
		// cancel would fire late by the running total of discarded audio.
		std::vector<u8> scrap;
		board.pull_samples(scrap, SAMPLE_RATE);
		samples_dropped += scrap.size();
		samples_dropped += carry.size() - carry_off; // pulled-but-undelivered remainder
		board.index_events().clear();
		carry.clear();
		carry_off = 0;
	}

	// Move bytes host->card while the card is RDY; stops (without spinning)
	// as soon as it is not.
	void feed()
	{
		while (!queue.empty() && board.rdy())
		{
			board.host_write(queue.front());
			queue.pop_front();
			// RDY drops 2-3us after a write and returns 180-190us later
			// (dtlk.h); run just past that so the next loop iteration can
			// feed the next byte instead of stalling to the next chunk
			board.run_cycles(2000);
		}
	}

	void pull()
	{
		board.pull_samples(carry, SAMPLE_RATE);
		auto &ev = board.index_events();
		while (!ev.empty())
		{
			u64 pos = u64(ev.front().first) * SAMPLE_RATE / doubletalk_board::CPU_HZ;
			// Absolute grid pos -> delivered-stream pos: subtract the boot
			// discard plus everything dtalk_stop has thrown away since.
			u64 offset = samples_base + samples_dropped;
			marks.push_back({ev.front().second, pos > offset ? pos - offset : 0});
			ev.pop_front();
		}
	}

	bool boot()
	{
		const s64 limit = s64(doubletalk_board::CPU_HZ) * 10;
		while (!board.rdy() && board.now_cycles() < limit)
			board.run_cycles(10000);
		if (!board.rdy())
			return false;
		// swallow power-on audio (the genuine hardware DC click)
		board.run_cycles(doubletalk_board::CPU_HZ / 10);
		std::vector<u8> scrap;
		board.pull_samples(scrap, SAMPLE_RATE);
		board.index_events().clear();
		samples_base = u64(board.now_cycles()) * SAMPLE_RATE / doubletalk_board::CPU_HZ;
		return true;
	}
};

extern "C" {

dtalk *dtalk_create(const void *rom, size_t rom_size)
{
	dtalk *dt = new dtalk;
	if (!dt->board.load_rom(static_cast<const u8 *>(rom), rom_size))
	{
		delete dt;
		return nullptr;
	}
	dt->board.reset();
	dt->save_rate_table(); // pristine period table, before any boost
	if (!dt->boot())
	{
		delete dt;
		return nullptr;
	}
	return dt;
}

void dtalk_destroy(dtalk *dt)
{
	delete dt;
}

void dtalk_reset(dtalk *dt)
{
	dt->queue.clear();
	dt->marks.clear();
	dt->carry.clear();
	dt->carry_off = 0;
	dt->samples_dropped = 0;
	dt->idle_stable = 0;
	dt->out16.reset();
	dt->board.reset();
	dt->boot();
}

uint32_t dtalk_sample_rate(const dtalk *)
{
	return SAMPLE_RATE;
}

int dtalk_rate_boost_max(void)
{
	return RATE_BOOST_MAX;
}

void dtalk_set_rate_boost(dtalk *dt, int level)
{
	dt->apply_rate_boost(level);
}

int dtalk_get_rate_boost(const dtalk *dt)
{
	return dt->rate_boost;
}

uint8_t dtalk_lpc_status(dtalk *dt)
{
	return dt->board.host_lpc_status();
}

void dtalk_queue(dtalk *dt, const void *bytes, size_t len)
{
	const u8 *p = static_cast<const u8 *>(bytes);
	dt->queue.insert(dt->queue.end(), p, p + len);
	dt->idle_stable = 0;
}

void dtalk_say(dtalk *dt, const char *text)
{
	dtalk_queue(dt, text, std::strlen(text));
	const u8 cr = 0x0d;
	dtalk_queue(dt, &cr, 1);
}

void dtalk_stop(dtalk *dt)
{
	dt->queue.clear();
	dt->marks.clear();
	// Ctrl-X written directly, handshake deliberately ignored (manual:
	// "the states of DoubleTalk's handshaking signals should be ignored
	// when writing the Clear command")
	dt->board.host_write(0x18);
	dt->board.run_cycles(RUN_CHUNK_CYCLES); // let the firmware react
	dt->drop_pending_audio();
	// Clear the output-stage filter history so a resume after this cancel does
	// not replay a discontinuity: prime=true makes the next real sample seed
	// the DC-block at that level, so no boundary step (and thus no re-click) is
	// synthesized across the cut.
	dt->out16.reset();
	dt->idle_stable = 0;
}

int dtalk_active(dtalk *dt)
{
	return (!dt->queue.empty() || dt->card_busy()) ? 1 : 0;
}

size_t dtalk_synth(dtalk *dt, uint8_t *out, size_t max_samples)
{
	size_t produced = 0;

	auto drain_carry = [&]()
	{
		size_t avail = dt->carry.size() - dt->carry_off;
		size_t take = std::min(avail, max_samples - produced);
		std::memcpy(out + produced, dt->carry.data() + dt->carry_off, take);
		produced += take;
		dt->carry_off += take;
		if (dt->carry_off == dt->carry.size())
		{
			dt->carry.clear();
			dt->carry_off = 0;
		}
	};

	drain_carry();

	while (produced < max_samples)
	{
		dt->feed();

		if (dt->queue.empty() && !dt->card_busy())
		{
			if (dt->idle_stable >= IDLE_SETTLE_CYCLES)
				break;
			dt->idle_stable += RUN_CHUNK_CYCLES;
		}
		else
			dt->idle_stable = 0;

		dt->board.run_cycles(RUN_CHUNK_CYCLES);
		dt->pull();
		drain_carry();
	}

	return produced;
}

size_t dtalk_synth16(dtalk *dt, int16_t *out, size_t max_samples)
{
	// Same sample-production loop as dtalk_synth, but each raw u8 grid sample
	// is run through the modeled output stage into a signed 16-bit sample.
	size_t produced = 0;

	auto drain_carry = [&]()
	{
		while (produced < max_samples && dt->carry_off < dt->carry.size())
			out[produced++] = dt->out16.process(
				(double(dt->carry[dt->carry_off++]) - 128.0) / 128.0);
		if (dt->carry_off == dt->carry.size())
		{
			dt->carry.clear();
			dt->carry_off = 0;
		}
	};

	drain_carry();

	while (produced < max_samples)
	{
		dt->feed();

		if (dt->queue.empty() && !dt->card_busy())
		{
			if (dt->idle_stable >= IDLE_SETTLE_CYCLES)
				break;
			dt->idle_stable += RUN_CHUNK_CYCLES;
		}
		else
			dt->idle_stable = 0;

		dt->board.run_cycles(RUN_CHUNK_CYCLES);
		dt->pull();
		drain_carry();
	}

	return produced;
}

void dtalk_set_lowpass_hz(dtalk *dt, uint32_t hz)
{
	// 0 restores the datasheet-authentic default; otherwise set_lowpass clamps
	// to LPF_HZ_MIN..LPF_HZ_MAX. Only the biquad coefficients change; the
	// filter's sample history is preserved so a mid-stream change is seamless.
	dt->out16.set_lowpass(hz == 0 ? LPF_HZ : double(hz));
}

size_t dtalk_read_index_marks(dtalk *dt, dtalk_index_mark *out, size_t max)
{
	size_t n = 0;
	while (n < max && !dt->marks.empty())
	{
		out[n++] = dt->marks.front();
		dt->marks.pop_front();
	}
	return n;
}

} // extern "C"
