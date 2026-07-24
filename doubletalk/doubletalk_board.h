// license:BSD-3-Clause
// copyright-holders:David Sexton
// Standalone RC Systems DoubleTalk PC board: the onboard 80C188EB (vendored
// MAME core, see mame/) plus the thin board wrapper ported from the MAME ISA
// driver (doubletalkpc.cpp in the companion MAME DoubleTalk PC driver - the
// reference implementation this port is validated against).
//
// The host-facing surface is the same two-port protocol a real ISA host
// sees: write text bytes to the TTS port (RDY-gated), read the TTS status
// byte. Audio comes out as timestamped 8-bit PCM writes from the firmware's
// timer ISR (CPU I/O port 0x00), which pull_samples() flattens into a
// constant-rate unsigned 8-bit stream (zero-order hold, like the real DAC).

#ifndef DOUBLETALK_BOARD_H
#define DOUBLETALK_BOARD_H

#pragma once

#include "emu.h"
#include "i186.h"

#include <deque>

class doubletalk_board;

// CPU subclass so the harness can trace instructions and inspect
// protected core state.
class doubletalk_cpu : public i80c188eb_cpu_device
{
public:
	doubletalk_cpu(const machine_config &mconfig, const char *tag, device_t *owner, u32 clock)
		: i80c188eb_cpu_device(mconfig, tag, owner, clock)
	{
	}

	bool halted() const { return m_halt; }
	u32 phys_pc() const { return (u32(m_sregs[CS]) << 4) + m_ip; }
	u16 reg_ip() const { return m_ip; }
	u16 sreg(int i) const { return m_sregs[i]; }
	u16 wreg(int i) const { return m_regs.w[i]; }

	// per-instruction trace hook (virtual in the shim only)
	std::function<void(offs_t)> trace_hook;
	virtual void debugger_instruction_hook(offs_t pc) override
	{
		if (trace_hook)
			trace_hook(pc);
	}
};

class doubletalk_board
{
public:
	static constexpr u32 XTAL_HZ = 20000000;      // stock 20MHz crystal
	static constexpr u32 CPU_HZ = XTAL_HZ / 2;    // 10MHz processor clock
	static constexpr u32 RAM_SIZE = 0x20000;      // 128KB (generous placeholder)
	static constexpr u32 ROM_BASE = 0x80000;
	static constexpr u32 ROM_SIZE = 0x80000;
	static constexpr u32 MAILBOX_ADDR = 0xa100;

	doubletalk_board();

	// Load the 512KB firmware image (doubletalkpc.bin); must be called
	// before reset(). Returns false on size mismatch.
	bool load_rom(const u8 *data, size_t size);

	void reset();

	// --- host (ISA) side, same semantics as the MAME driver ---
	// TTS data port write: latch into the mailbox and raise INT1 (stays
	// asserted until the firmware's ISR reads the mailbox).
	void host_write(u8 data);
	// TTS status port read (firmware-owned byte from CPU port 0x40):
	// TTS_READABLE 0x80 / SYNC 0x40 / SYNC2 0x20 / RDY 0x10 / AF 0x08 / AE 0x04
	u8 host_status() const { return m_tts_status; }
	bool rdy() const { return (m_tts_status & 0x10) != 0; }

	// LPC status/data port read (base+0, firmware-owned byte from CPU port
	// 0x80): 0x7F when idle. This is the port a real ISA host reads to
	// probe for the card - Linux dtlk.c's word read at the base port must
	// come back 0x107f (low byte 0x7f = LPC idle, high byte 0x10 = TTS RDY)
	// for the card to be detected, and dtlk_readable()/dtlk_read_lpc() treat
	// any value != 0x7f as an available (index-marker) byte to read.
	u8 host_lpc_status() const { return m_lpc_status; }

	// --- execution ---
	// Advance emulated time by (at least) this many CPU cycles.
	s64 run_cycles(s64 cycles) { return m_machine.run_cycles(cycles); }
	s64 now_cycles() const { return m_machine.now_cycles(); }

	// --- audio ---
	// Number of raw DAC write events captured so far (diagnostics).
	size_t dac_event_count() const { return m_dac_total_events; }
	// Raw not-yet-flattened DAC events (diagnostics).
	const std::deque<std::pair<s64, u8>> &dac_events() const { return m_dac_events; }

	// Index-marker events (cycle, marker number) from firmware port 0x80
	// writes; consumer drains this.
	std::deque<std::pair<s64, u8>> &index_events() { return m_index_events; }

	// Little-endian 16-bit read of CPU program space RAM (diagnostics /
	// firmware-state peeking, e.g. the 0x000F/0x0011 buffer pointers).
	u16 ram16(u32 addr) const { return u16(m_ram[addr]) | u16(m_ram[addr + 1]) << 8; }
	// Poke a RAM byte (diagnostics / firmware-state injection).
	void ram_poke(u32 addr, u8 v) { if (addr < RAM_SIZE) m_ram[addr] = v; }
	// Read/poke the in-memory ROM copy (diagnostics / in-RAM ROM patching).
	// addr is a ROM-space offset (0..ROM_SIZE); the firmware sees the patched
	// bytes as if burned in. Never touches the on-disk file.
	u8 rom_peek(u32 addr) const { return addr < ROM_SIZE ? m_rom[addr] : 0xff; }
	void rom_poke(u32 addr, u8 v) { if (addr < ROM_SIZE) m_rom[addr] = v; }
	// Optional per-RAM-access watch hook (addr, is_write, data). When set,
	// dt_program_space calls it on every RAM byte access - debug use only,
	// leave null in production paths.
	std::function<void(offs_t, bool, u8)> ram_access_hook;
	// Flatten DAC events up to current emulated time into unsigned 8-bit
	// PCM at sample_rate_hz (zero-order hold), appending to out.
	void pull_samples(std::vector<u8> &out, u32 sample_rate_hz);

	doubletalk_cpu &cpu() { return *m_cpu; }

private:
	friend class dt_program_space;
	friend class dt_io_space;

	u8 mailbox_read();
	void io_write(u16 port, u8 data);
	u8 io_read(u16 port);

	running_machine m_machine;
	machine_config m_mconfig{m_machine};
	std::unique_ptr<doubletalk_cpu> m_cpu;
	std::unique_ptr<address_space> m_program_space;
	std::unique_ptr<address_space> m_io_space;

	std::vector<u8> m_ram;
	std::vector<u8> m_rom;

	u8 m_tts_status = 0;
	// Host-visible LPC status/data latch (base+0), fed by firmware CPU port
	// 0x80 writes. Idle value 0x7F - see host_lpc_status()/dtlk.c probe.
	u8 m_lpc_status = 0x7f;
	u8 m_mailbox_data = 0;
	bool m_mailbox_pending = false;

	// DAC events not yet flattened into output samples
	std::deque<std::pair<s64, u8>> m_dac_events;
	// index-marker events not yet consumed
	std::deque<std::pair<s64, u8>> m_index_events;
	size_t m_dac_total_events = 0;
	u8 m_dac_level = 0x80;          // last value written (ZOH hold level)
	s64 m_audio_emitted_cycles = 0; // emulated time already covered by pull_samples
};

#endif // DOUBLETALK_BOARD_H
