// license:BSD-3-Clause
// Minimal MAME-compatibility shim so the vendored, unmodified i86.cpp/i186.cpp
// CPU core (see ../mame/) compiles and runs outside MAME.
//
// Design notes (see doubletalk_notes/PORTING.md):
//  - There is exactly one clocked device in this system (the 80C188EB), so
//    attotime is represented directly as a count of that CPU's cycles.
//    cycles_to_attotime()/attotime_to_cycles() are identities and timer
//    arithmetic is exact integer math - no drift, no rounding.
//  - The MAME event scheduler is replaced by a flat list of emu_timers plus a
//    run loop (running_machine::run_cycles) that sizes each execute_run()
//    timeslice to end exactly at the next timer expiry. A timer adjusted from
//    inside CPU execution to expire before the current slice ends aborts the
//    slice (same as MAME's abort_timeslice), so expiry stays exact to the
//    instruction boundary.
//  - Debugger, savestate, and register-view plumbing (state_add/save_item/
//    debugger_*_hook) are inert stubs. debugger_instruction_hook is virtual
//    here (unlike MAME) so a harness subclass can hook per-instruction tracing.

#ifndef DOUBLETALK_SHIM_EMU_H
#define DOUBLETALK_SHIM_EMU_H

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using offs_t = u32;

#include "endianness.h"

#define ATTR_COLD

enum endianness_t
{
	ENDIANNESS_LITTLE,
	ENDIANNESS_BIG
};

template <typename T, typename U> constexpr T BIT(T x, U n) noexcept { return (x >> n) & T(1); }
constexpr u16 swapendian_int16(u16 v) noexcept { return u16((v << 8) | (v >> 8)); }

// input line states / numbers
enum
{
	CLEAR_LINE = 0,
	ASSERT_LINE = 1,
	HOLD_LINE = 2
};

enum
{
	INPUT_LINE_IRQ0 = 0,
	INPUT_LINE_IRQ1 = 1,
	INPUT_LINE_NMI = 61,
	INPUT_LINE_RESET = 62,
	INPUT_LINE_HALT = 63
};

// debugger state ids
enum
{
	STATE_GENPC = -1,
	STATE_GENPCBASE = -2,
	STATE_GENFLAGS = -3
};

// address space ids
enum
{
	AS_PROGRAM = 0,
	AS_DATA = 1,
	AS_IO = 2,
	AS_OPCODES = 3
};

#define ACCESSING_BITS_0_7 ((mem_mask & 0x000000ffU) != 0)
#define ACCESSING_BITS_8_15 ((mem_mask & 0x0000ff00U) != 0)

#define NAME(x) x
#define STRUCT_MEMBER(s, m) s
#define FUNC(x) (&x)
#define IRQ_CALLBACK_MEMBER(name) int name(device_t &device, int irqline)
#define TIMER_CALLBACK_MEMBER(name) void name(s32 param)

[[noreturn]] void fatalerror(const char *fmt, ...);

// printf-style stand-in for util::string_format (only used for debugger
// register views in the vendored core)
std::string string_format(const char *format, ...);

// ------------------------------------------------------------------------
// attotime: a count of CPU cycles of the single emulated CPU
// ------------------------------------------------------------------------

struct attotime
{
	s64 m_cycles = 0;

	attotime() = default;
	constexpr explicit attotime(s64 cycles) : m_cycles(cycles) { }

	static const attotime never;
	static const attotime zero;

	constexpr bool is_never() const { return m_cycles == INT64_MAX; }
	double as_double() const;

	constexpr attotime operator+(const attotime &o) const { return attotime(m_cycles + o.m_cycles); }
	constexpr attotime operator-(const attotime &o) const { return attotime(m_cycles - o.m_cycles); }
	constexpr bool operator<(const attotime &o) const { return m_cycles < o.m_cycles; }
	constexpr bool operator<=(const attotime &o) const { return m_cycles <= o.m_cycles; }
	constexpr bool operator>(const attotime &o) const { return m_cycles > o.m_cycles; }
	constexpr bool operator>=(const attotime &o) const { return m_cycles >= o.m_cycles; }
	constexpr bool operator==(const attotime &o) const { return m_cycles == o.m_cycles; }
	constexpr bool operator!=(const attotime &o) const { return m_cycles != o.m_cycles; }
};

// ------------------------------------------------------------------------
// forward declarations
// ------------------------------------------------------------------------

class device_t;
class cpu_device;
class running_machine;
class emu_timer;

// ------------------------------------------------------------------------
// device type registration
// ------------------------------------------------------------------------

struct device_type_impl
{
	const char *shortname;
	const char *fullname;
};
using device_type = const device_type_impl &;

#define DECLARE_DEVICE_TYPE(Type, Class) \
	class Class; \
	extern const device_type_impl Type;
#define DEFINE_DEVICE_TYPE(Type, Class, ShortName, FullName) \
	const device_type_impl Type{ShortName, FullName};

struct machine_config
{
	running_machine &m_machine;
};

// ------------------------------------------------------------------------
// address spaces
// ------------------------------------------------------------------------

struct address_space_config
{
	address_space_config(const char *name, endianness_t endian, int data_width, int addr_width, int addr_shift = 0)
		: m_name(name), m_endian(endian), m_data_width(data_width), m_addr_width(addr_width), m_addr_shift(addr_shift)
	{
	}

	const char *m_name;
	endianness_t m_endian;
	int m_data_width;
	int m_addr_width;
	int m_addr_shift;
};

using space_config_vector = std::vector<std::pair<int, const address_space_config *>>;

struct device_memory_interface
{
	using space_config_vector = ::space_config_vector;
};

// The harness implements read_byte/write_byte; word accessors decompose into
// byte accesses, which is exactly right for the 8-bit external bus of an
// 80188-family part.
class address_space
{
public:
	virtual ~address_space() = default;

	virtual u8 read_byte(offs_t addr) = 0;
	virtual void write_byte(offs_t addr, u8 data) = 0;

	u16 read_word(offs_t addr) { return u16(read_byte(addr)) | u16(read_byte(addr + 1)) << 8; }
	u16 read_word_unaligned(offs_t addr) { return read_word(addr); }
	void write_word(offs_t addr, u16 data) { write_byte(addr, u8(data)); write_byte(addr + 1, u8(data >> 8)); }
	void write_word(offs_t addr, u16 data, u16 mem_mask)
	{
		if (mem_mask & 0x00ff)
			write_byte(addr, u8(data));
		if (mem_mask & 0xff00)
			write_byte(addr + 1, u8(data >> 8));
	}
	void write_word_unaligned(offs_t addr, u16 data) { write_word(addr, data); }

	int data_width() const { return 8; }

	template <typename Cache> void cache(Cache &c) { c.set(this); }
};

template <int AddrBits, int DataShift, int AddrShift, endianness_t Endian>
struct memory_access
{
	struct cache
	{
		address_space *m_space = nullptr;
		void set(address_space *space) { m_space = space; }
		u8 read_byte(offs_t addr) { return m_space->read_byte(addr); }
		u16 read_word(offs_t addr) { return m_space->read_word(addr); }
	};
	using specific = cache;
};

// ------------------------------------------------------------------------
// device callback stubs (nothing on the DoubleTalk board uses these)
// ------------------------------------------------------------------------

class devcb_write_line
{
public:
	devcb_write_line(device_t &) { }
	void operator()(int) { }
	int bind() { return 0; }
};

class devcb_write16
{
public:
	devcb_write16(device_t &) { }
	void operator()(u16) { }
	void operator()(offs_t, u16) { }
	void operator()(offs_t, u16, u16) { }
	int bind() { return 0; }
};

class devcb_write32
{
public:
	devcb_write32(device_t &) { }
	void operator()(u32) { }
	void operator()(offs_t, u32) { }
	int bind() { return 0; }
};

class devcb_read8
{
public:
	devcb_read8(device_t &, u8 default_value = 0) : m_default(default_value) { }
	u8 operator()(offs_t = 0) { return m_default; }
	int bind() { return 0; }

private:
	u8 m_default;
};

class device_irq_acknowledge_delegate
{
public:
	device_irq_acknowledge_delegate(device_t &) { }
	template <typename... T> void set(T &&...) { }
	void resolve_safe(int default_value) { m_default = default_value; }
	int operator()(device_t &, int) { return m_default; }

private:
	int m_default = 0;
};

// ------------------------------------------------------------------------
// debugger state stubs
// ------------------------------------------------------------------------

class device_state_entry
{
public:
	int index() const { return m_index; }
	int m_index = 0;
};

struct state_entry_stub
{
	state_entry_stub &formatstr(const char *) { return *this; }
	state_entry_stub &callimport() { return *this; }
	state_entry_stub &callexport() { return *this; }
	state_entry_stub &mask(u64) { return *this; }
	state_entry_stub &noshow() { return *this; }
};

// ------------------------------------------------------------------------
// disassembler interface stub
// ------------------------------------------------------------------------

#include <iosfwd>

namespace util {

class disasm_interface
{
public:
	struct data_buffer
	{
		virtual ~data_buffer() = default;
	};

	virtual ~disasm_interface() = default;
	virtual u32 opcode_alignment() const = 0;
	virtual offs_t disassemble(std::ostream &stream, offs_t pc, const data_buffer &opcodes, const data_buffer &params) = 0;
};

} // namespace util

// ------------------------------------------------------------------------
// timers and machine
// ------------------------------------------------------------------------

class emu_timer
{
public:
	void adjust(attotime duration, s32 param = 0);
	bool enabled() const { return m_enabled; }
	attotime remaining() const;
	attotime expire() const { return attotime(m_expire); }
	s32 param() const { return m_param; }

private:
	friend class running_machine;

	running_machine *m_machine = nullptr;
	std::function<void(s32)> m_callback;
	s64 m_expire = INT64_MAX;
	s32 m_param = 0;
	bool m_enabled = false;
};

class running_machine
{
public:
	running_machine() = default;
	running_machine(const running_machine &) = delete;
	~running_machine();

	attotime time() const;
	std::string describe_context() const;

	// --- shim harness API (not part of the MAME surface) ---

	void set_cpu(cpu_device *cpu) { m_cpu = cpu; }
	cpu_device *cpu() const { return m_cpu; }

	// Run the machine forward by (at least) `cycles` CPU cycles, firing due
	// timers at their exact expiry. Returns cycles actually consumed (can
	// overshoot by up to one instruction).
	s64 run_cycles(s64 cycles);

	s64 now_cycles() const;

	emu_timer *alloc_timer(std::function<void(s32)> callback);
	void timer_list_changed(emu_timer *changed);

	// CPU-cycles-per-second of the one clocked device; used only for
	// double-precision time formatting.
	static void set_cycles_per_second(double cps);

private:
	s64 next_timer_expiry() const;
	void fire_due_timers();

	cpu_device *m_cpu = nullptr;
	std::vector<emu_timer *> m_timers;

	s64 m_base_cycles = 0;      // total cycles consumed before the current slice
	bool m_in_timer_cb = false;
	s64 m_timer_cb_time = 0;    // machine time while inside a timer callback
	bool m_in_cpu_slice = false;
	s64 m_slice_end = 0;        // absolute cycle count the current slice aims for
};

// ------------------------------------------------------------------------
// devices
// ------------------------------------------------------------------------

class device_t
{
public:
	device_t(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock)
		: m_machine(mconfig.m_machine), m_type(type), m_tag(tag), m_owner(owner), m_clock(clock)
	{
	}
	virtual ~device_t() = default;

	running_machine &machine() const { return m_machine; }
	const char *tag() const { return m_tag; }
	u32 clock() const { return m_clock; }

	void logerror(const char *format, ...) const;

	template <typename T> void save_item(T &&, int = 0) { }

	template <typename T = void, typename... A> state_entry_stub &state_add(A &&...)
	{
		static state_entry_stub stub;
		return stub;
	}

	// harness entry points into the protected MAME lifecycle
	void shim_start() { device_start(); }
	void shim_reset() { device_reset(); }

protected:
	virtual void device_start() { }
	virtual void device_reset() { }

	template <typename D> emu_timer *timer_alloc(void (D::*fn)(s32), D *dev)
	{
		return machine().alloc_timer([dev, fn](s32 param) { (dev->*fn)(param); });
	}

private:
	running_machine &m_machine;
	device_type m_type;
	const char *m_tag;
	device_t *m_owner;
	u32 m_clock;
};

// Merges MAME's cpu_device with the execute/memory/state/disasm interfaces
// the vendored core overrides.
class cpu_device : public device_t
{
public:
	cpu_device(const machine_config &mconfig, device_type type, const char *tag, device_t *owner, u32 clock)
		: device_t(mconfig, type, tag, owner, clock)
	{
	}

	// --- execute interface ---
	virtual u32 execute_min_cycles() const noexcept { return 1; }
	virtual u32 execute_max_cycles() const noexcept { return 1; }
	virtual bool execute_input_edge_triggered(int) const noexcept { return false; }
	virtual u64 execute_clocks_to_cycles(u64 clocks) const noexcept { return clocks; }
	virtual u64 execute_cycles_to_clocks(u64 cycles) const noexcept { return cycles; }
	virtual void execute_run() = 0;
	virtual void execute_set_input(int, int) { }

	void set_input_line(int line, int state) { execute_set_input(line, state); }

	attotime cycles_to_attotime(u64 cycles) const { return attotime(s64(cycles)); }
	s64 attotime_to_cycles(const attotime &t) const { return t.m_cycles; }

	void set_icountptr(int &icount) { m_icountptr = &icount; }

	template <typename D> void set_irq_acknowledge_callback(D &obj, int (D::*fn)(device_t &, int))
	{
		m_irq_ack = [&obj, fn](device_t &device, int irqline) { return (obj.*fn)(device, irqline); };
	}

	int standard_irq_callback(int irqline, u32)
	{
		if (m_irq_ack)
			return m_irq_ack(*this, irqline);
		return 0;
	}

	s64 total_cycles() const;

	// IOCHRDY wait-state machinery: no device on the DoubleTalk board
	// wait-states the CPU, so accesses never need redoing.
	bool access_to_be_redone() { return false; }

	// --- memory interface ---
	virtual space_config_vector memory_space_config() const = 0;

	void shim_set_space(int index, address_space *space)
	{
		assert(index >= 0 && index < 4);
		m_spaces[index] = space;
	}
	address_space &space(int index) const
	{
		assert(m_spaces[index]);
		return *m_spaces[index];
	}
	bool has_space(int index) const { return m_spaces[index] != nullptr; }
	bool has_configured_map(int index) const { return m_spaces[index] != nullptr; }

	// --- debugger/state stubs ---
	virtual void state_import(const device_state_entry &) { }
	virtual void state_string_export(const device_state_entry &, std::string &) const { }
	virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }

	// Virtual in the shim (plain function in MAME) so a harness subclass can
	// trace each instruction.
	virtual void debugger_instruction_hook(offs_t) { }
	void debugger_wait_hook() { }
	void debugger_exception_hook(int) { }

	// --- shim run-loop plumbing (used by running_machine) ---
	int *shim_icountptr() const { return m_icountptr; }
	s64 shim_slice_base = 0;   // icount value the current slice started with
	void abort_timeslice()
	{
		if (m_icountptr && *m_icountptr > 0)
		{
			shim_slice_base -= *m_icountptr;
			*m_icountptr = 0;
		}
	}

private:
	int *m_icountptr = nullptr;
	std::function<int(device_t &, int)> m_irq_ack;
	address_space *m_spaces[4] = {nullptr, nullptr, nullptr, nullptr};
};

#endif // DOUBLETALK_SHIM_EMU_H
