// license:BSD-3-Clause
#include "doubletalk_board.h"

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
			return m_board.m_ram[addr];
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
			m_board.m_ram[addr] = data;
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
	m_mailbox_data = 0;
	m_mailbox_pending = false;
	m_dac_events.clear();
	m_dac_level = 0x80;
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
	default:
		// port 0x80 "multi-mode board control latch" and anything else:
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

void doubletalk_board::pull_samples(std::vector<u8> &out, u32 sample_rate_hz)
{
	// Emit ZOH samples covering [m_audio_emitted_cycles, now). Sample n
	// (counting from power-on) is taken at cycle n * CPU_HZ / rate; use
	// 128-bit-safe integer math via u64 (cycles < 2^63, rate fits easily).
	const s64 now = m_machine.now_cycles();
	s64 next_sample_index = (m_audio_emitted_cycles * s64(sample_rate_hz) + s64(CPU_HZ) - 1) / s64(CPU_HZ);

	for (;;)
	{
		s64 sample_cycle = next_sample_index * s64(CPU_HZ) / s64(sample_rate_hz);
		if (sample_cycle >= now)
			break;
		while (!m_dac_events.empty() && m_dac_events.front().first <= sample_cycle)
		{
			m_dac_level = m_dac_events.front().second;
			m_dac_events.pop_front();
		}
		out.push_back(m_dac_level);
		next_sample_index++;
	}
	m_audio_emitted_cycles = now;
}
