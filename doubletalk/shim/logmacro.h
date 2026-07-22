// license:BSD-3-Clause
// copyright-holders:David Sexton
// Shim replacement for MAME's logmacro.h: all masked logging compiles out.
#ifndef DOUBLETALK_SHIM_LOGMACRO_H
#define DOUBLETALK_SHIM_LOGMACRO_H

#pragma once

#define LOGMASKED(mask, ...) do { } while (0)
#define LOG(...) do { } while (0)

#endif // DOUBLETALK_SHIM_LOGMACRO_H
