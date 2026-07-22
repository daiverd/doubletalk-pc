// license:BSD-3-Clause
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
constexpr double LPF_HZ = 3000.0;       // reconstruction low-pass corner.
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

// Firmware text-buffer read/write pointers in CPU RAM (same addresses the
// MAME capture.lua watched): equal <=> input buffer fully consumed.
constexpr u32 BUF_READ_PTR = 0x000f;
constexpr u32 BUF_WRITE_PTR = 0x0011;

// Idle must hold this long (emulated) before we call the stream finished -
// covers the firmware's own latency between consuming text and raising
// SYNC when audio starts.
constexpr s64 IDLE_SETTLE_CYCLES = doubletalk_board::CPU_HZ * 3 / 20; // 150ms

constexpr s64 RUN_CHUNK_CYCLES = doubletalk_board::CPU_HZ / 100; // 10ms

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
		// 2-pole Butterworth low-pass, bilinear transform.
		const double w = std::tan(DT_PI * LPF_HZ / double(SAMPLE_RATE));
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
