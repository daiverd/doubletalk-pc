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
// constant-rate unsigned 8-bit stream (band-limited resampling - the DAC clock
// is not the output grid and the firmware retunes it; see pull_samples).

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
	// Firmware timer-ISR cadence at the default formant setting (5F): one
	// port-0x00 DAC write per ~952 CPU cycles. 952 is the MEAN, not a
	// constant - measured over 19,990 intervals of real speech the period
	// ranges 907..998 cycles (mean 951.999, std 5.35, 43 distinct values;
	// the modal 952 is only 15% of them). That spread is the firmware's own
	// interrupt latency, and it is jitter rather than drift.
	//
	// The MEAN moves, though: the nF formant-frequency command is implemented
	// purely as a DAC sample-clock change - varispeed. Measured, the timer
	// reload is 952 - 12*(F-5), and the emitted sample VALUES are byte
	// identical across all ten settings:
	//
	//     F=0  1012 cycles   9882.8 Hz      F=7   928 cycles  10775.1 Hz
	//     F=2   988 cycles  10122.3 Hz      F=9   904 cycles  11060.4 Hz
	//     F=5   952 cycles  10504.1 Hz  (default)
	//
	// nF is the only command that does this; A/B/E/P/R/S/V/X all hold
	// 951.999 cycles and change the samples instead. So the board's output
	// rate is NOT fixed, and pull_samples() must genuinely resample.
	static constexpr u32 DAC_ISR_CYCLES = 952;
	static constexpr u32 DAC_HZ = CPU_HZ / DAC_ISR_CYCLES; // 10504
	// Longest DAC period the firmware can be driving (0F, plus jitter margin).
	// Sizes the lookahead pull_samples() holds back so its kernel always has
	// real samples to its right rather than an edge-extended guess.
	static constexpr u32 DAC_ISR_CYCLES_MAX = 1024;
	// Half-width of the resampling kernel, in source samples (see
	// pull_samples). 8 -> 16 taps, whose stopband is far below the 8-bit DAC's
	// own noise floor; the cost is 16 multiply-adds per output sample.
	static constexpr int RESAMP_HALF = 8;
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
	// writes; consumer drains this. Cycles, not sample counts: the output
	// grid is a fixed rate the caller chooses while the DAC clock varies with
	// the formant setting, so cycles are the only stable axis both sides
	// agree on.
	std::deque<std::pair<s64, u8>> &index_events() { return m_index_events; }

	// Absolute count of output samples pull_samples() has emitted since
	// power-on.
	u64 samples_emitted() const { return m_dac_samples_emitted; }

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
	// Resample the queued DAC events up to current emulated time onto a fixed
	// sample_rate_hz grid of unsigned 8-bit PCM, appending to out.
	//
	// This is a real rate conversion, not a formality: the source rate is the
	// firmware's DAC clock, which jitters +/-5 cycles and which the nF
	// formant command retunes over 9883..11060 Hz (see DAC_ISR_CYCLES), while
	// the grid the caller asks for is fixed. Converting a variable source
	// rate to a fixed output rate is exactly what makes nF audible as
	// varispeed - drop this and formant becomes a no-op.
	//
	// The conversion is a windowed-sinc (Lanczos, RESAMP_HALF taps each side)
	// interpolation on the SOURCE SAMPLE INDEX axis: DAC write k is sample k of
	// a sequence clocked at the firmware's ISR rate, each output instant is
	// located as a fractional index between the writes bracketing it, and the
	// kernel is evaluated at that fractional offset. When the source is faster
	// than the grid (7F/9F) the kernel's cutoff is scaled down to the output
	// Nyquist so decimation stays anti-aliased.
	//
	// Two cheaper conversions were tried and both are audibly wrong:
	//
	//   Nearest-preceding-write beats the 952.02-cycle grid against the
	//   ~952-cycle writes and slips ~33 times a second at 5F, dropping one
	//   write and repeating its neighbour each time.
	//
	//   Two-point linear interpolation fixes the slipping but is not
	//   band-limited: its response is sinc^2, which at 5kHz costs 7.0dB
	//   against a hold's 3.5dB, and - because that loss depends on where in
	//   the source period the output instant happens to land - it MODULATES
	//   with the grid/DAC phase. Since consecutive utterances start at
	//   unrelated phases, the same word rendered twice came out up to 0.74dB
	//   apart, heard as the level of an /s/ wobbling between repetitions.
	//
	// A band-limited kernel has neither problem: gain is flat to the passband
	// edge regardless of phase, so repeated utterances match and the highs
	// survive. This is genuine rate conversion, not a formality - the source
	// rate jitters +/-5 cycles and the nF formant command retunes it over
	// 9883..11060Hz (see DAC_ISR_CYCLES), which is exactly what makes nF
	// audible as varispeed.
	//
	// Output lags `now` by RESAMP_HALF source periods so the kernel always has
	// real samples to its right; pass drain=true to flush that tail (clamping
	// the missing right-hand taps to the last DAC level, which is what the
	// real DAC holds anyway). Callers that discard the result - boot, cancel -
	// want drain=true so nothing is left pending to surface later.
	void pull_samples(std::vector<u8> &out, u32 sample_rate_hz, bool drain = false);

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
	u64 m_dac_samples_emitted = 0;  // absolute output samples emitted so far
	u8 m_dac_level = 0x80;          // level held before the first event arrives
	// Resampler cursor. m_dac_events is NOT drained down to the current output
	// instant: the kernel reaches RESAMP_HALF samples either side, so events
	// stay queued until they fall off the left edge of its support.
	// m_dac_cursor indexes the bracket-left event within m_dac_events.
	size_t m_dac_cursor = 0;
	s64 m_next_sample_index = 0;    // absolute index of the next grid sample
};

#endif // DOUBLETALK_BOARD_H
