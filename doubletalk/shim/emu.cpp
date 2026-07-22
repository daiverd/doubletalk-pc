// license:BSD-3-Clause
// copyright-holders:David Sexton
// Implementation of the MAME-compatibility shim's timer scheduler and run
// loop. See emu.h for the design notes.

#include "emu.h"

const attotime attotime::never = attotime(INT64_MAX);
const attotime attotime::zero = attotime(0);

// The one machine instance's cycles_per_second is needed to render an
// attotime as seconds; there is only ever one machine in this process's
// use of the shim, so a process-wide value is fine.
static double s_cycles_per_second = 10000000.0;

void running_machine::set_cycles_per_second(double cps)
{
	s_cycles_per_second = cps;
}

double attotime::as_double() const
{
	if (is_never())
		return 1e30;
	return double(m_cycles) / s_cycles_per_second;
}

std::string string_format(const char *format, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, format);
	std::vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);
	return buf;
}

s64 cpu_device::total_cycles() const
{
	return machine().now_cycles();
}

[[noreturn]] void fatalerror(const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	std::vfprintf(stderr, format, ap);
	va_end(ap);
	std::fputc('\n', stderr);
	std::abort();
}

void device_t::logerror(const char *format, ...) const
{
	static const bool enabled = std::getenv("DTALK_LOGERROR") != nullptr;
	if (!enabled)
		return;
	va_list ap;
	va_start(ap, format);
	std::vfprintf(stderr, format, ap);
	va_end(ap);
}

// ------------------------------------------------------------------------
// emu_timer
// ------------------------------------------------------------------------

void emu_timer::adjust(attotime duration, s32 param)
{
	m_param = param;
	if (duration.is_never())
	{
		m_enabled = false;
		m_expire = INT64_MAX;
	}
	else
	{
		m_enabled = true;
		m_expire = m_machine->now_cycles() + duration.m_cycles;
	}
	m_machine->timer_list_changed(this);
}

attotime emu_timer::remaining() const
{
	if (!m_enabled)
		return attotime::never;
	return attotime(m_expire - m_machine->now_cycles());
}

// ------------------------------------------------------------------------
// running_machine
// ------------------------------------------------------------------------

running_machine::~running_machine()
{
	for (emu_timer *t : m_timers)
		delete t;
}

emu_timer *running_machine::alloc_timer(std::function<void(s32)> callback)
{
	emu_timer *t = new emu_timer;
	t->m_machine = this;
	t->m_callback = std::move(callback);
	m_timers.push_back(t);
	return t;
}

s64 running_machine::now_cycles() const
{
	if (m_in_timer_cb)
		return m_timer_cb_time;
	if (m_in_cpu_slice && m_cpu && m_cpu->shim_icountptr())
		return m_base_cycles + (m_cpu->shim_slice_base - *m_cpu->shim_icountptr());
	return m_base_cycles;
}

attotime running_machine::time() const
{
	return attotime(now_cycles());
}

std::string running_machine::describe_context() const
{
	char buf[64];
	std::snprintf(buf, sizeof(buf), "[t=%lld]", (long long)now_cycles());
	return buf;
}

void running_machine::timer_list_changed(emu_timer *changed)
{
	// MAME's abort_timeslice equivalent: if a timer was just scheduled to
	// expire before the end of the CPU slice we're inside, end the slice
	// after the current instruction so the expiry is honored exactly.
	if (m_in_cpu_slice && !m_in_timer_cb && changed->m_enabled && changed->m_expire < m_slice_end)
		m_cpu->abort_timeslice();
}

s64 running_machine::next_timer_expiry() const
{
	s64 next = INT64_MAX;
	for (const emu_timer *t : m_timers)
		if (t->m_enabled)
			next = std::min(next, t->m_expire);
	return next;
}

void running_machine::fire_due_timers()
{
	// Fire every timer whose expiry has been reached, earliest first, with
	// machine time pinned to each timer's exact expiry during its callback
	// (matching MAME, where a callback runs "at" its scheduled time even if
	// the CPU overshot it by part of an instruction).
	for (;;)
	{
		emu_timer *due = nullptr;
		for (emu_timer *t : m_timers)
			if (t->m_enabled && t->m_expire <= m_base_cycles && (!due || t->m_expire < due->m_expire))
				due = t;
		if (!due)
			return;

		m_in_timer_cb = true;
		m_timer_cb_time = due->m_expire;
		due->m_enabled = false;
		due->m_expire = INT64_MAX;
		due->m_callback(due->m_param);
		m_in_timer_cb = false;
	}
}

s64 running_machine::run_cycles(s64 cycles)
{
	assert(m_cpu && m_cpu->shim_icountptr());
	const s64 start = m_base_cycles;
	const s64 target = start + cycles;

	// Cap on a single execute_run() call so a HALTed CPU with no pending
	// timer still returns to the caller regularly.
	constexpr s64 MAX_SLICE = 100000;

	while (m_base_cycles < target)
	{
		fire_due_timers();

		s64 slice = std::min(target, next_timer_expiry()) - m_base_cycles;
		slice = std::min(std::max<s64>(slice, 1), MAX_SLICE);

		int &icount = *m_cpu->shim_icountptr();
		icount = int(slice);
		m_cpu->shim_slice_base = slice;
		m_slice_end = m_base_cycles + slice;

		m_in_cpu_slice = true;
		m_cpu->execute_run();
		m_in_cpu_slice = false;

		// consumed can exceed the requested slice by part of an instruction,
		// or fall short if the slice was aborted (shim_slice_base already
		// re-based in that case).
		s64 consumed = m_cpu->shim_slice_base - icount;
		assert(consumed >= 0);
		m_base_cycles += consumed;
	}

	fire_due_timers();
	return m_base_cycles - start;
}
