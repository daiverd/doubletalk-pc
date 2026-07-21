// license:BSD-3-Clause
// C API implementation over doubletalk_board. See dtalk.h.

#define DTALK_BUILD 1
#include "dtalk.h"

#include "doubletalk_board.h"

#include <deque>

namespace {

// Firmware timer cadence: one DAC write per 952 CPU cycles (measured).
constexpr u32 SAMPLE_RATE = doubletalk_board::CPU_HZ / 952; // 10504

// Firmware text-buffer read/write pointers in CPU RAM (same addresses the
// MAME capture.lua watched): equal <=> input buffer fully consumed.
constexpr u32 BUF_READ_PTR = 0x000f;
constexpr u32 BUF_WRITE_PTR = 0x0011;

// Idle must hold this long (emulated) before we call the stream finished -
// covers the firmware's own latency between consuming text and raising
// SYNC when audio starts.
constexpr s64 IDLE_SETTLE_CYCLES = doubletalk_board::CPU_HZ * 3 / 20; // 150ms

constexpr s64 RUN_CHUNK_CYCLES = doubletalk_board::CPU_HZ / 100; // 10ms

} // namespace

struct dtalk
{
	doubletalk_board board;
	std::deque<u8> queue;              // host-side bytes not yet accepted by the card
	std::vector<u8> carry;             // pulled but not yet delivered samples
	size_t carry_off = 0;
	std::deque<dtalk_index_mark> marks;
	u64 samples_base = 0;              // board-grid samples discarded at boot
	s64 idle_stable = 0;               // consecutive idle cycles observed

	bool card_busy() const
	{
		return (board.host_status() & 0x40) != 0 // SYNC: speech in progress
			|| board.ram16(BUF_READ_PTR) != board.ram16(BUF_WRITE_PTR);
	}

	void drop_pending_audio()
	{
		std::vector<u8> scrap;
		board.pull_samples(scrap, SAMPLE_RATE);
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
			marks.push_back({ev.front().second, pos > samples_base ? pos - samples_base : 0});
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
	dt->idle_stable = 0;
	dt->board.reset();
	dt->boot();
}

uint32_t dtalk_sample_rate(const dtalk *)
{
	return SAMPLE_RATE;
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
