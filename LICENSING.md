# Licensing & Attribution

Everything in this repository is **BSD-3-Clause** (our own code plus a vendored
MAME CPU core), except a proprietary firmware ROM that is **not included**. This
document states, for every part of the tree, what license applies and who holds
the copyright.

**Summary**

- All **original code we wrote** is **BSD-3-Clause** — free to use, modify, and
  redistribute (including commercially) as long as you keep the copyright notice
  and don't use our names to endorse your product. It does **not** require you to
  open-source the rest of your project.
- The **vendored CPU emulation** from MAME is also **BSD-3-Clause**.
- The card's **Linux kernel drivers** (`dtlk.c`, `speakup_dtlk.c`) were used as a
  protocol reference during development but are **not included here** — they are
  GPL-2.0 and live upstream in the Linux source (the notes link to them).
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
| `docs/**`, `notes/**`, `*.md` | Our documentation and research notes | BSD-3-Clause | David Sexton (except `notes/investigation-audio-path.md`: **Christopher Toth**) |
| `doubletalkpc.bin` (the ROM) | DoubleTalk PC firmware | **Proprietary — not included** | RC Systems, Inc. |

Every source file carries a matching `license:` / `copyright-holders:` header
(MAME-style `// license:` for C/C++, `# license:` for scripts/manifests).

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

## Protocol reference — the Linux kernel drivers (external, not included)

The card's host I/O protocol was reverse-engineered against the upstream Linux
kernel drivers for the physical DoubleTalk card. Those drivers are **GPL-2.0 and
are not included in this repository** — they live in the Linux source tree:

- `drivers/char/dtlk.c` + `include/linux/dtlk.h` — the DoubleTalk PC/LT character
  driver (© Chris Pallotta, Jim Van Zandt).
- `drivers/accessibility/speakup/speakup_dtlk.c` — the Speakup driver
  (© Kirk Reiser, David Borowski).

Browse them at e.g.
<https://elixir.bootlin.com/linux/v5.15/source/drivers/char/dtlk.c>. The notes
cite these as the protocol ground truth; nothing here is built from or links
against them, so the repository itself is entirely BSD-3-Clause.

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
