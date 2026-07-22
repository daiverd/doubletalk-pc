// license:BSD-3-Clause
// copyright-holders:David Sexton
// Shim stub for MAME's x86 disassembler: i86.cpp constructs one in
// create_disassembler(), but nothing in the standalone harness disassembles
// through it, so it is inert.
#ifndef DOUBLETALK_SHIM_I386DASM_H
#define DOUBLETALK_SHIM_I386DASM_H

#pragma once

#include "emu.h"

class i386_disassembler : public util::disasm_interface
{
public:
	class config
	{
	public:
		virtual ~config() = default;
		virtual int get_mode() const = 0;
	};

	i386_disassembler(config *conf) : m_config(conf) { }

	virtual u32 opcode_alignment() const override { return 1; }
	virtual offs_t disassemble(std::ostream &, offs_t, const data_buffer &, const data_buffer &) override { return 1; }

private:
	config *m_config;
};

#endif // DOUBLETALK_SHIM_I386DASM_H
