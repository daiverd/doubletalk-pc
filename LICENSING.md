# Licensing & Attribution

This repository aggregates code under a few different licenses, plus a
proprietary firmware ROM that is **not included**. This document states, for
every part of the tree, what license applies and who holds the copyright.

**Summary**

- All **original code we wrote** is **BSD-3-Clause** — free to use, modify, and
  redistribute (including commercially) as long as you keep the copyright notice
  and don't use our names to endorse your product. It does **not** require you to
  open-source the rest of your project.
- The **vendored CPU emulation** from MAME is also **BSD-3-Clause**.
- The **Linux kernel driver** under `driver/` is **GPL-2.0** third-party code,
  included only as hardware-interface reference. It is **not compiled into or
  linked with** the BSD-licensed emulator, so its copyleft does not extend to
  the rest of the repo (mere aggregation).
- The **DoubleTalk firmware ROM is proprietary to RC Systems and is not
  distributed here.** You supply your own (see [The firmware ROM](#the-firmware-rom)).

## Component map

| Path | What it is | License | Copyright holder(s) |
|---|---|---|---|
| `doubletalk/shim/**` | MAME-compatibility shim (our code) | BSD-3-Clause | David Sexton |
| `doubletalk/doubletalk_board.{cpp,h}` | Standalone board wrapper (our code) | BSD-3-Clause | David Sexton |
| `doubletalk/dtalk.{cpp,h}`, `dtalk_cli.cpp` | C API + CLI (our code) | BSD-3-Clause | David Sexton |
| `doubletalk/Makefile` | Build (our code) | BSD-3-Clause | David Sexton |
| `doubletalk/nvda/**` | NVDA add-on driver, manifest, build script (our code) | BSD-3-Clause | David Sexton |
| `doubletalk/mame/i86.*`, `i186.*`, `i86inline.h` | Vendored MAME 8086/80186 + 80C188EB CPU core | BSD-3-Clause | Carl; **Christopher Toth** (I80C188EB / EB Peripheral Control Block additions) |
| `doubletalk/mame/endianness.h` | Vendored MAME utility | BSD-3-Clause | Aaron Giles, Vas Crabb |
| `driver/dtlk.c`, `driver/dtlk.h` | Linux kernel DoubleTalk driver (reference) | **GPL-2.0** | Chris Pallotta, Jim Van Zandt |
| `driver/speakup_dtlk.c` | Linux Speakup DoubleTalk driver (reference) | **GPL-2.0-or-later** | Kirk Reiser, David Borowski |
| `docs/**`, `notes/**`, `*.md` | Our documentation and research notes | BSD-3-Clause | David Sexton |
| `doubletalkpc.bin` (the ROM) | DoubleTalk PC firmware | **Proprietary — not included** | RC Systems, Inc. |

Every source file carries a matching `license:` / `copyright-holders:` header
(MAME-style `// license:` for C/C++, `# license:` for scripts/manifests, SPDX
tags for the kernel files).

## Original project code — BSD-3-Clause

Everything we wrote (`doubletalk/shim`, the board wrapper, the `dtalk` C API and
CLI, the Makefile, and the NVDA add-on) is BSD-3-Clause, © David Sexton. The
full license text is in [`LICENSE`](LICENSE). This means you may use it in
commercial and closed-source projects; you only have to preserve the copyright
notice and license text and refrain from using the authors' names to endorse
your product.

## Vendored MAME CPU core — BSD-3-Clause

`doubletalk/mame/` contains CPU-emulation source redistributed from
[MAME](https://github.com/mamedev/mame), unchanged in license (BSD-3-Clause).
The base 8086/80186 core is © **Carl**; the **Intel 80C188EB** device and its
EB-mode Peripheral Control Block support (the `i80c188eb_cpu_device`, the
`write_memory_byte_al` shared-core hook, `m_eb_registers`, etc.) were authored by
**Christopher Toth** and his name is appended to the `copyright-holders:` line of
each file he modified, per MAME convention. `endianness.h` is © Aaron Giles and
Vas Crabb. These files retain their original MAME headers verbatim.

## Linux kernel driver — GPL-2.0 (third-party reference)

`driver/dtlk.c` and `driver/dtlk.h` (GPL-2.0) and `driver/speakup_dtlk.c`
(GPL-2.0-or-later) are the upstream Linux kernel drivers for the physical
DoubleTalk card, included **only as a reference** for the card's I/O protocol.
They are **not** part of the emulator build and are **not** linked with any
BSD-licensed code here. Because they are merely stored alongside (not combined
into a single program with) the BSD code, this is "mere aggregation" under the
GPL and the GPL does **not** apply to the rest of the repository. If you reuse
these specific files, GPL-2.0 terms apply to them.

## NVDA add-on — how BSD and GPL coexist

The NVDA add-on (`doubletalk/nvda/`) is our BSD-3-Clause code that imports GPL
NVDA modules at runtime. NVDA's license (`copying.txt`) grants an explicit
exception allowing plugins and drivers under other (non-GPL) licenses, and
BSD-3-Clause is in any case one-way compatible into GPL. So our driver stays
BSD-3-Clause, and the assembled add-on is distributable. The prebuilt DLLs
(`dtalk.dll` / `dtalk64.dll`, when present) are built with mingw-w64 g++ using
`-static-libgcc -static-libstdc++`; the statically linked GCC runtime is covered
by the GCC Runtime Library Exception and imposes no additional obligations.

## The firmware ROM

`doubletalkpc.bin` (512 KB) is the DoubleTalk PC firmware. It is **proprietary
to RC Systems, Inc., carries no redistribution grant, and is not included in
this repository or in any release artifact.** RC Systems is still an active
company; do not treat this ROM as abandonware.

Following standard MAME practice, this project **identifies the ROM by hash but
never distributes it and gives no sourcing guidance.** You must supply the ROM
yourself — for example by dumping it from a DoubleTalk PC card you own — and
place it where the tools expect it (see the NVDA add-on README and the CLI
usage). Verify your dump against:

- **CRC32:** `66685631`
- **SHA1:** `bf7e78d6381c76d291ee069971873347a314ffff`

The NVDA driver's `check()` returns `False` when the ROM is absent, so a ROM-less
install simply won't offer the synthesizer rather than crashing. **Do not commit
the ROM** (the repository `.gitignore` blocks `*.bin` / `rom/` to help prevent
this).
