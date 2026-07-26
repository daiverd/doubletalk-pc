// license:BSD-3-Clause
// copyright-holders:David Sexton
#include "doubletalk_board.h"

#include <algorithm>
#include <cmath>

// ------------------------------------------------------------------------
// address spaces (port of doubletalkpc.cpp's cpu_map/cpu_io)
// ------------------------------------------------------------------------

class dt_program_space : public address_space
{
public:
	dt_program_space(doubletalk_board &board) : m_board(board) { }

	virtual u8 read_byte(offs_t addr) override
	{
		addr &= 0xfffff;
		if (addr == doubletalk_board::MAILBOX_ADDR)
			return m_board.mailbox_read();
		if (addr < doubletalk_board::RAM_SIZE)
		{
			if (m_board.ram_access_hook)
				m_board.ram_access_hook(addr, false, m_board.m_ram[addr]);
			return m_board.m_ram[addr];
		}
		if (addr >= doubletalk_board::ROM_BASE)
			return m_board.m_rom[addr - doubletalk_board::ROM_BASE];
		return 0;
	}

	virtual void write_byte(offs_t addr, u8 data) override
	{
		addr &= 0xfffff;
		if (addr == doubletalk_board::MAILBOX_ADDR)
		{
			// Host-to-CPU only as far as traced; store rather than drop
			// (matches the MAME driver's mailbox_w).
			m_board.m_mailbox_data = data;
			return;
		}
		if (addr < doubletalk_board::RAM_SIZE)
		{
			if (m_board.ram_access_hook)
				m_board.ram_access_hook(addr, true, data);
			m_board.m_ram[addr] = data;
		}
		// ROM and unmapped writes: sink
	}

private:
	doubletalk_board &m_board;
};

class dt_io_space : public address_space
{
public:
	dt_io_space(doubletalk_board &board) : m_board(board) { }

	virtual u8 read_byte(offs_t addr) override { return m_board.io_read(u16(addr)); }
	virtual void write_byte(offs_t addr, u8 data) override { m_board.io_write(u16(addr), data); }

private:
	doubletalk_board &m_board;
};

// ------------------------------------------------------------------------
// doubletalk_board
// ------------------------------------------------------------------------

doubletalk_board::doubletalk_board()
{
	m_ram.resize(RAM_SIZE, 0);
	m_rom.resize(ROM_SIZE, 0xff);

	m_cpu = std::make_unique<doubletalk_cpu>(m_mconfig, "doubletalkpc_cpu", nullptr, XTAL_HZ);
	m_program_space = std::make_unique<dt_program_space>(*this);
	m_io_space = std::make_unique<dt_io_space>(*this);

	m_cpu->shim_set_space(AS_PROGRAM, m_program_space.get());
	m_cpu->shim_set_space(AS_IO, m_io_space.get());
	m_machine.set_cpu(m_cpu.get());
	running_machine::set_cycles_per_second(double(CPU_HZ));

	m_cpu->shim_start();
}

bool doubletalk_board::load_rom(const u8 *data, size_t size)
{
	if (size != ROM_SIZE)
		return false;
	std::memcpy(m_rom.data(), data, size);
	return true;
}

void doubletalk_board::reset()
{
	m_cpu->shim_reset();
	m_tts_status = 0x00; // firmware initializes the real value via CPU port 0x40
	// LPC status/data latch idle value. The firmware re-drives this to 0x7f
	// via CPU port 0x80 early in boot (traced at ~t=1.05M cycles), but seed
	// it here too so a host probe before that write still reads the idle
	// sentinel Linux dtlk.c's dtlk_dev_probe() expects (0x107f word at base).
	m_lpc_status = 0x7f;
	m_mailbox_data = 0;
	m_mailbox_pending = false;
	m_dac_events.clear();
	m_index_events.clear();
	m_dac_level = 0x80;
	m_dac_cursor = 0;
}

void doubletalk_board::host_write(u8 data)
{
	// Overrun silently loses the previous unconsumed byte, matching real
	// hardware and the MAME driver; a well-behaved host polls RDY first.
	m_mailbox_data = data;
	m_mailbox_pending = true;
	// INT1 doorbell: assert and leave asserted until mailbox_read()
	// (read-triggered deassert is a hard requirement - see PORTING.md).
	m_cpu->int1_w(1);
}

u8 doubletalk_board::mailbox_read()
{
	// The ISR at 0x81D26 reads this address as its first instruction for
	// every byte, so this is the reliable point to deassert INT1.
	m_mailbox_pending = false;
	m_cpu->int1_w(0);
	return m_mailbox_data;
}

void doubletalk_board::io_write(u16 port, u8 data)
{
	static const bool log_io = std::getenv("DTALK_LOG_IO") != nullptr;
	if (log_io && port != 0x00)
		std::fprintf(stderr, "[t=%lld] io_write %04x <- %02x\n",
			(long long)m_machine.now_cycles(), port, data);
	switch (port)
	{
	case 0x00:
		// unsigned 8-bit PCM sample from the firmware's timer ISR
		m_dac_events.emplace_back(m_machine.now_cycles(), data);
		m_dac_total_events++;
		break;
	case 0x40:
		// firmware-owned host-visible TTS status byte
		m_tts_status = data;
		break;
	case 0x80:
		// Host-visible LPC status/data latch (base+0). The firmware writes
		// 0x7f here as the idle sentinel (once early in boot), and writes a
		// marker number when speech output reaches an embedded Ctrl-A <n> I
		// index marker - the same latch a real ISA host reads for card
		// detection (0x7f idle) and index-mark readout (dtlk.c's
		// dtlk_read_lpc: value != 0x7f means a byte is available). Latch it
		// for host_lpc_status(), and (unchanged) surface the non-idle marker
		// writes as index events for the streaming consumer.
		m_lpc_status = data;
		m_index_events.emplace_back(m_machine.now_cycles(), data);
		break;
	default:
		// harmless write sink (matches the MAME driver)
		m_cpu->logerror("io_write: unmapped port %04x <- %02x\n", port, data);
		break;
	}
}

u8 doubletalk_board::io_read(u16 port)
{
	m_cpu->logerror("io_read: unmapped port %04x\n", port);
	return 0;
}

namespace {

// Lanczos kernel: sinc(x) * sinc(x/a) for |x| < a, 0 outside. `cutoff` scales
// the kernel in frequency (1.0 = source Nyquist); values below 1 narrow the
// passband so a source running faster than the output grid is band-limited
// before it is decimated.
inline double lanczos(double x, double cutoff, int a)
{
	if (x <= -a || x >= a)
		return 0.0;
	if (x == 0.0)
		return cutoff;
	constexpr double PI = 3.14159265358979323846;
	const double px = PI * x;
	// sinc(cutoff*x) * cutoff, times the Lanczos window sinc(x/a)
	return cutoff * (std::sin(px * cutoff) / (px * cutoff)) * (std::sin(px / a) / (px / a));
}

} // anonymous namespace

void doubletalk_board::pull_samples(std::vector<u8> &out, u32 sample_rate_hz, bool drain)
{
	const s64 now = m_machine.now_cycles();

	// Hold back a lookahead margin so the kernel's right-hand taps are real
	// samples rather than an edge-extended guess. drain=true gives that up to
	// flush the tail - correct at end of utterance, where the ISR has stopped
	// and the DAC is genuinely holding its last level anyway.
	const s64 margin = drain ? 0 : s64(RESAMP_HALF) * s64(DAC_ISR_CYCLES_MAX);
	const s64 horizon = now - margin;

	for (;;)
	{
		// Sample n (counting from power-on) is taken at cycle n*CPU_HZ/rate.
		const s64 sample_cycle = m_next_sample_index * s64(CPU_HZ) / s64(sample_rate_hz);
		if (sample_cycle >= horizon)
			break;

		// Advance the cursor to the last event at or before this instant.
		while (m_dac_cursor + 1 < m_dac_events.size()
			&& m_dac_events[m_dac_cursor + 1].first <= sample_cycle)
			m_dac_cursor++;

		u8 value;
		if (m_dac_events.empty() || m_dac_events[m_dac_cursor].first > sample_cycle)
		{
			// Before the first event ever captured: the DAC is still sitting
			// at its power-on level.
			value = m_dac_events.empty() ? m_dac_level : m_dac_events.front().second;
		}
		else
		{
			const size_t j = m_dac_cursor;
			const s64 t0 = m_dac_events[j].first;
			const bool have_right = (j + 1 < m_dac_events.size());
			const s64 span = have_right ? (m_dac_events[j + 1].first - t0) : 0;

			// A bracket wider than two DAC periods is not a sample interval,
			// it is the silence between utterances (the ISR stops). Band-
			// limited reconstruction across that would ring; the real DAC just
			// holds, so hold. Same when there is no right-hand event yet.
			if (span <= 0 || span > s64(DAC_ISR_CYCLES) * 2)
			{
				value = m_dac_events[j].second;
			}
			else
			{
				// Fractional position on the source-sample-index axis.
				const double frac = double(sample_cycle - t0) / double(span);
				// Local source rate is CPU_HZ/span; band-limit to whichever of
				// source and output Nyquist is lower.
				const double out_period = double(CPU_HZ) / double(sample_rate_hz);
				const double cutoff = std::min(1.0, double(span) / out_period);

				double acc = 0.0, wsum = 0.0;
				for (int k = -RESAMP_HALF + 1; k <= RESAMP_HALF; k++)
				{
					const double w = lanczos(frac - k, cutoff, RESAMP_HALF);
					if (w == 0.0)
						continue;
					// Clamp taps past either end of the queue to the edge
					// sample (the DAC held that level).
					const s64 idx = s64(j) + k;
					const size_t ci = size_t(std::clamp<s64>(idx, 0, s64(m_dac_events.size()) - 1));
					acc += w * double(m_dac_events[ci].second);
					wsum += w;
				}
				// Normalize: the tap weights of a windowed sinc do not sum to
				// exactly 1 at an arbitrary fractional offset, and the residual
				// would show up as broadband gain ripple.
				const double v = (wsum != 0.0) ? (acc / wsum) : double(m_dac_events[j].second);
				value = u8(std::clamp(int(std::lround(v)), 0, 255));
			}
		}

		out.push_back(value);
		m_dac_samples_emitted++;
		m_next_sample_index++;
	}

	// Retire events that have fallen off the left edge of the kernel support.
	// Keep RESAMP_HALF behind the cursor so the next call can still reach them.
	const size_t keep_behind = size_t(RESAMP_HALF);
	if (m_dac_cursor > keep_behind)
	{
		const size_t drop = m_dac_cursor - keep_behind;
		m_dac_level = m_dac_events[drop - 1].second;
		m_dac_events.erase(m_dac_events.begin(), m_dac_events.begin() + drop);
		m_dac_cursor -= drop;
	}
}
