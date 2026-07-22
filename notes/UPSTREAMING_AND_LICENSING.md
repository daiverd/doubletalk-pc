# DoubleTalk PC: Upstreaming to MAME & Distribution Licensing

Research notes covering (1) how to contribute the MAME ISA-card emulation
upstream and (2) what of the standalone emulator + NVDA add-on can legally be
distributed. Written 2026-07-22. Sources: MAME contributing docs
(docs.mamedev.org/contributing, mamedev/mame CONTRIBUTING.md), the working
`doubletalk` branch in `~/src/mame`, NVDA `copying.txt` and add-on docs, and the
files under `native/retrochip/doubletalk/`.

---

## QUESTION 1 — Upstreaming the DoubleTalk PC driver to MAME

### 1.1 What is actually on the branch (`git diff master..doubletalk --stat`)

Upstreamable code:

| File | Change | Upstream? |
|---|---|---|
| `src/devices/bus/isa/doubletalkpc.cpp` (+200) | New ISA card device | Yes |
| `src/devices/bus/isa/doubletalkpc.h` (+91) | New device header | Yes |
| `src/devices/bus/isa/isa_cards.cpp` (+2) | Slot registration | Yes |
| `scripts/src/bus.lua` (+2) | Build hookup | Yes |
| `src/devices/cpu/i86/i186.cpp` (+73) | I80C188EB subtype + EB Peripheral Control Block | Yes (see split below) |
| `src/devices/cpu/i86/i186.h` (+14) | EB device type / ctor / state | Yes |
| `src/devices/cpu/i86/i86.cpp` (+7) | New `write_memory_byte_al()` virtual; route `MOV [disp],AL` through it | Yes (shared core) |
| `src/devices/cpu/i86/i86.h` (+1) | Declares the virtual | Yes |

**NOT** upstreamed (research artifacts — leave behind):
`DOUBLETALK.md`, `investigations/*.md`, `roms/*.zip` (the ROM dumps — never
committed to MAME), and everything under `scripts/` that is trace/regression
tooling (`capture.lua`, `doubletalk_trace_phrase.lua`,
`doubletalk_regression_declaration.lua`, `run_doubletalk_regression.py`). The
only `scripts/` change that ships is the two-line `scripts/src/bus.lua` build
hookup.

### 1.2 Recommended PR structure — split into two

The work touches two independently reviewed subsystems with different
maintainers, so submit as **two PRs**, the CPU one first:

**PR 1 — i86/i186: add I80C188EB with EB-mode Peripheral Control Block.**
This is the higher-risk change because `i86.cpp`'s new
`write_memory_byte_al()` virtual alters the shared `i8086_common_cpu_device`
opcode path for `MOV moffs,AL` (opcode 0xA2) for *every* 8086-family CPU, not
just the EB. It refactors the opcode to compute the effective address and call
a virtual so the 80186-derived core can trap writes that land in the memory-
mapped PCB window. Reviewers will want to confirm this is behaviour-neutral for
existing 8086/80186/V20/etc. drivers (it should be: base class just does
`write_byte(addr, AL)`). The EB support itself (I80C188EB device type, the
`m_eb_peripherals` flag flipping the reset relocation register to 0x00ff, the
`internal_port_offset()` PCB-register translation table, and the `m_eb_registers`
backing store for untranslated EB regs) is a real, reusable Intel part and is
genuinely useful to MAME independent of this card. Land it on its own so the
CPU maintainers can vet it without the ISA card in the way.

**PR 2 — bus/isa: RC Systems DoubleTalk PC card.** Depends on PR 1. Adds
`doubletalkpc.cpp/.h`, the `isa_cards.cpp` slot option, and the `bus.lua`
build entry. Small and self-contained once the CPU is in.

Rationale for the order/split is exactly the "touches a shared CPU core"
concern in the task: a change to `i8086_common_cpu_device` gets more scrutiny
and shouldn't be blocked on (or block) the card review.

### 1.3 Licensing / copyright headers — **action required**

MAME requires every source file to open with two comment lines:

```
// license:BSD-3-Clause
// copyright-holders:Name One, Name Two
```

- The new device files already carry `// license:BSD-3-Clause` but the
  **`// copyright-holders:` line is EMPTY** in both `doubletalkpc.cpp` and
  `doubletalkpc.h`. This **must be filled in before a PR** — MAME's CI/srcclean
  and reviewers reject empty attribution.
- Attribution per the branch history (`git log master..doubletalk`):
  **Christopher Toth `<q.alpha@gmail.com>` (14 commits, incl. all the I80C188EB
  peripheral work on `i186.cpp`) and David Sexton `<daiverd@gmail.com>`
  (4 commits, incl. the card scaffold/driver).** So:
  - `doubletalkpc.cpp/.h` → `// copyright-holders:Christopher Toth, David Sexton`
    (adjust to real-name display forms both are comfortable with; MAME lists
    handles or names — e.g. Toth's contributions could read `ctoth` if that's
    his preferred MAME attribution).
- The i86/i186 files already read `// copyright-holders:Carl` (i86*, i186) and
  `Aaron Giles, Vas Crabb` (endianness.h); `i186.cpp` also credits Phill
  Harvey-Smith / Aaron Giles / Paul Leaman in a comment. When editing these,
  **append** new authors to the existing `copyright-holders:` line, never
  replace — e.g. `// copyright-holders:Carl, Christopher Toth`. The EB work is
  substantial enough to justify adding the author there.
- BSD-3-Clause is MAME's default and correct here — no action beyond filling
  in the holders.

### 1.4 The proprietary ROM

Standard MAME practice, and this driver already follows it: `ROM_START` names
the file and pins it by CRC/SHA1 but **MAME never distributes the ROM image**:

```
ROM_LOAD( "doubletalkpc.bin", 0x0000, 0x80000,
          CRC(66685631) SHA1(bf7e78d6381c76d291ee069971873347a314ffff) )
```

This is exactly how every other MAME driver references copyrighted ROMs, so it
is upstream-clean as written. The user supplies the dump themselves. Provenance
note for the PR description: the dump used for development came from an
archive.org copy (CRC32 66685631); include the SHA1 so MAME's audit tooling and
any future redumper can verify. Do **not** attach the ROM to the PR or a
softlist.

### 1.5 Practical reviewer checklist

Solid / ready:
- Device inherits `device_t` + `device_isa8_card_interface` correctly, uses
  `DEFINE_DEVICE_TYPE` / `DECLARE_DEVICE_TYPE`, `device_add_mconfig` wiring
  (`I80C188EB` at `20_MHz_XTAL`, `DAC_8BIT_R2R`, speaker), and `ATTR_COLD` on
  cold methods — all idiomatic.
- ROM_START/CRC/SHA1 handling is standard.
- The i86 base-class refactor is minimal and behaviour-neutral.

Needs polish before PR:
- **logerror verbosity.** Every port access calls `logerror(...)`
  unconditionally. MAME convention for chatty per-access tracing is the
  `logmacro.h` channel system (`#define VERBOSE 0` at top, `LOGMASKED(...)`),
  and the C++ guidelines explicitly say to ship with `VERBOSE 0`. Convert the
  trace `logerror`s to `LOGMASKED` (or drop them) so a default build is quiet.
- **Hardcoded I/O base 0x25e.** `device_start()` installs at `0x025e` and the
  header lists the six jumper-selectable bases (0x25e/0x29e/0x2de/0x31e/0x35e/
  0x39e). Real hardware is jumper-selectable; reviewers generally want this
  exposed via a DIP/`input_ports`/config rather than hardcoded. Currently a
  known limitation (comment says "Hardcoded to 0x25e for now").
- **Unconfirmed RAM map.** `cpu_map` sizes RAM below the ROM as a "placeholder"
  (comment: "RAM size below that is not confirmed - sized generously here").
  Reviewers will ask for a rationale comment or a tighter map; keep the honest
  comment but be ready to defend the layout (reset vector at 0xFFFF0, ROM based
  at 0x80000).
- **Device short-name convention.** `DEFINE_DEVICE_TYPE(ISA8_DOUBLETALKPC,
  ..., "doubletalkpc_isa", "RC Systems DoubleTalk PC")`. The isa8 precedent
  (`dectalk`) uses a bare `"dectalk"` short name and slot option; the slot
  option here is already `"doubletalkpc"` (good, matches dectalk style), but the
  device short name is `"doubletalkpc_isa"`. Minor — either is accepted — but
  consider dropping the `_isa` suffix on the short name to match the `dectalk`
  precedent for consistency. Internal CPU tag `"doubletalkpc_cpu"` /
  `m_cpu(*this,"doubletalkpc_cpu")` is fine.

CI / hygiene MAME reviewers expect:
- **Builds clean in their CI.** Enable GitHub Actions on your fork; MAME builds
  Windows/Linux/macOS/clang and treats warnings as errors. The device must
  compile under all.
- **Run `srcclean`** (MAME's whitespace/encoding normaliser) over every touched
  file before submitting — tabs for indent, no trailing whitespace, LF endings.
- **Descriptive commits + PR description.** MAME wants clear messages saying
  what's affected. **AI-assistance disclosure is mandatory**: MAME's
  CONTRIBUTING requires any PR that used AI assistance to state so in the PR
  description.
- No `GAME`/`COMP`/`CONS` line needed (this is a bus device, not a system), so
  the anonymous-namespace guidance for system drivers doesn't apply; the device
  is correctly exposed via its header.

### 1.6 Softlist / documentation

- **No softlist.** Softlists are for swappable media (cartridges, disks, tapes).
  This is a fixed-firmware peripheral, so there is nothing to list. The firmware
  lives in `ROM_START`, which is correct.
- **Documentation entry: optional, not required.** MAME won't block on it. A
  one-line mention in the ISA device roster / `docs` would be a courtesy but is
  not expected for a single card. The substantive documentation belongs in the
  PR description and the in-file header comment (which is already thorough).

---

## QUESTION 2 — What can be legally distributed

### 2.1 Per-component license inventory (from the actual file headers)

| Component | Files | License (from header) | Copyright holder |
|---|---|---|---|
| Vendored MAME CPU core | `mame/i86.cpp`, `i86.h`, `i86inline.h`, `i186.cpp`, `i186.h` | BSD-3-Clause | Carl (i186.cpp comment also credits Phill Harvey-Smith, Aaron Giles, Paul Leaman) |
| Vendored MAME util | `mame/endianness.h` | BSD-3-Clause | Aaron Giles, Vas Crabb |
| Vendored disasm stub | `shim/cpu/i386/i386dasm.h` | BSD-3-Clause | (MAME-derived stub) |
| Our MAME-compat shim | `shim/emu.cpp`, `emu.h`, `logmacro.h` | BSD-3-Clause | David Sexton (repo author) — **no `copyright-holders` line** |
| Our board wrapper | `doubletalk_board.cpp/.h` | BSD-3-Clause | David Sexton — no holder line |
| Our C API + CLI | `dtalk.cpp`, `dtalk.h`, `dtalk_cli.cpp` | BSD-3-Clause | David Sexton — no holder line |
| NVDA synth driver | `nvda/synthDrivers/doubletalkpc/__init__.py` | (no explicit license header) imports GPL NVDA modules | David Sexton |
| Built DLLs | `dtalk.dll`, `dtalk64.dll` | BSD-3-Clause sources, statically-linked GCC runtime | David Sexton (binary) |
| **Firmware ROM** | `doubletalkpc.bin` (512 KB, CRC32 66685631) | **Proprietary, no license grant** | **RC Systems** |

Everything we wrote and everything we vendored from MAME is **BSD-3-Clause**.
The only non-free artifact is the firmware ROM.

### 2.2 What BSD-3-Clause requires when we redistribute

For **both source and binary** redistribution, BSD-3 requires:
1. Retain the copyright notice, the license text, and the disclaimer — in
   source, in the source files; in binary, "in the documentation and/or other
   materials provided with the distribution."
2. Do not use the copyright holders' / contributors' names to endorse or
   promote derived products without permission (clause 3).

Practical consequence: ship a `LICENSE`/`copying` file (or a NOTICE) with any
binary distribution of the DLLs that reproduces the BSD-3 text and the MAME
authors' copyright notices (Carl; Aaron Giles; Vas Crabb; Phill Harvey-Smith;
Paul Leaman) alongside our own. This is a trivial obligation — one text file.

**Housekeeping:** our own files (`shim/*`, `doubletalk_board.*`, `dtalk.*`)
carry `// license:BSD-3-Clause` but **no `copyright-holders`/name line**. Add
one (David Sexton) so the attribution the license expects is actually present.

### 2.3 NVDA add-on: GPL analysis — **our BSD-3 code is fine, no relicensing forced**

NVDA itself is **GPL v2 or later**, but its `copying.txt` grants **two special
exceptions**, and the first is directly on point:

> Plugins and drivers may use components under other (non-GPL) licenses,
> provided they don't prevent GPL v2+ compliance and aren't directly used by
> NVDA code outside that plugin/driver.

So:
- Our `__init__.py` imports GPL NVDA modules (`config`, `nvwave`,
  `synthDriverHandler`, `speech.commands`, `logHandler`, `autoSettingsUtils`).
  In the general GPL case a distributed add-on that links GPL modules would be a
  derivative work and the combined distributable would have to be offered under
  GPL-compatible terms. **BSD-3 is one-way compatible into GPL** (you can
  combine BSD-3 code into a GPL work; the combined work is distributed under
  GPL), so even under the strict reading there is no conflict.
- But NVDA's explicit plugin/driver exception means our BSD-3 Python driver and
  our BSD-3 DLLs may sit inside the add-on **under their own BSD-3 license** —
  we are not *forced* to relicense our code GPL. The effective license of the
  combined add-on is: our parts BSD-3, NVDA's API GPL v2+, and the whole is
  distributable because the exception permits exactly this arrangement.
- Recommendation: keep our code BSD-3, and add a `license` note to the add-on's
  readme/manifest making the split explicit (our code BSD-3; interoperates with
  GPL NVDA under NVDA's plugin exception). The `__init__.py` should get a short
  BSD-3 header like the C++ files (it currently has none). NVDA's add-on store
  does not itself impose a specific license (the store *dataset* is ODC-PDDL;
  submissions aren't mandated GPL), so BSD-3 is acceptable for store submission.

### 2.4 The DLLs and the GCC runtime — redistributable

`dtalk.dll` / `dtalk64.dll` are built with mingw-w64 g++ using
`-static-libgcc -static-libstdc++`, so libgcc and libstdc++ are linked in
statically. Those runtimes are GPLv3 **with the GCC Runtime Library Exception**,
which explicitly permits distributing the statically-linked runtime with a
program compiled by GCC (as long as you're not defeating the exception with a
non-"Eligible" compilation process — a normal g++ build is eligible). So the
static GCC/libstdc++ runtime inside the DLLs is redistributable and does **not**
impose GPL on our BSD-3 code. No dynamic runtime DLLs need shipping either.
(NVDA's second exception separately whitelists Microsoft's MSVC runtime DLLs,
which we don't use here.)

### 2.5 The firmware ROM — must never ship, and it is currently bundled

`doubletalkpc.bin` is proprietary RC Systems firmware with **no license grant**.
It is **not distributable**. RC Systems (rcsys.com, Austin TX; DoubleTalk
family) appears to **still exist as a going concern** — the search surfaced a
current `rcsys.com/Downloads/pc104.pdf` DoubleTalk PC/104 product sheet — so
this is not even a plausible-abandonware situation; treat the ROM as fully
protected proprietary code. (Do not rely on abandonware theories as if they were
law; they aren't.)

**The problem:** the currently built package **bundles the ROM.** `unzip -l
nvda/doubletalkpc.nvda-addon` shows:

```
synthDrivers/doubletalkpc/doubletalkpc.bin   524288 bytes   ← the 512KB ROM
synthDrivers/doubletalkpc/dtalk.dll
synthDrivers/doubletalkpc/dtalk64.dll
synthDrivers/doubletalkpc/__init__.py
```

`nvda/build_addon.sh` **requires** `doubletalkpc.bin` to be present and zips it
in. So the current `.nvda-addon` on disk, and any package that script produces
as written, **must not be distributed publicly.** This directly contradicts the
add-on's own docstring/README, which claim the ROM is "proprietary … not
distributed with the add-on" — the intent is right, the packaging doesn't match
it.

**Fix for a public release:** ship the add-on **without** `doubletalkpc.bin`;
have the user supply it (fetch per `scripts/fetch_roms.sh`, drop it next to
`__init__.py`). Change `build_addon.sh` to exclude the ROM (and stop requiring
it), and document the manual ROM step in the readme.

**The driver already degrades gracefully — confirmed by reading the code.**
`SynthDriver.check()`:

```python
@classmethod
def check(cls):
    return os.path.isfile(os.path.join(_DIR, _dllName())) \
        and os.path.isfile(os.path.join(_DIR, "doubletalkpc.bin"))
```

If the ROM is absent, `check()` returns `False`, so NVDA simply doesn't list or
load the driver — no crash. (`_DtalkDLL.__init__` also `open()`s the ROM and
would raise if missing, but `check()` gates construction, so the missing-ROM
path is handled cleanly before we get there.) A ROM-less public package is
therefore safe and functional-once-the-user-adds-the-ROM.

### 2.6 Other proprietary-manual-derived content — checked, all clear

Facts are not copyrightable, and only facts were used:
- Voice names (`Perfect Paul`, `Vader`, `Big Bob`, `Precise Pete`, `Ricochet`,
  `Biff`, `Skip`, `Robo Robert`) — attributed "per the DoubleTalk PC/LT manual
  (Table 1)". These are short factual voice labels / names, not protectable
  expressive text. Fine.
- Command tables (`Ctrl-A nS/nP/nV/nO/nI`, ranges, the Ctrl-X clear, the status
  bit layout from the Linux `dtlk` driver) — these are interface facts. Fine.
- No verbatim manual prose was copied into the code, headers, or README. The
  comments paraphrase behaviour ("the manual notes it loads a preset that
  resets pitch/tone") rather than quoting. **No manual text needs removing.**
  If any longer verbatim passage from the manual were ever added to docs, that
  *would* be copyrightable and should be paraphrased — none is present today.

---

## Bottom line

**Safe to distribute publicly (with a BSD-3 NOTICE file):**
- All our source: `shim/*`, `doubletalk_board.*`, `dtalk.*`, `dtalk_cli.cpp`,
  `nvda/.../__init__.py`, build scripts.
- Vendored MAME CPU core + shim (BSD-3) — retaining the MAME copyright notices.
- The built DLLs (`dtalk.dll`, `dtalk64.dll`) — static GCC runtime is covered by
  the GCC Runtime Library Exception.
- The NVDA add-on **repackaged without the ROM** — BSD-3 code interoperating
  with GPL NVDA is permitted under NVDA's plugin/driver exception; `check()`
  already handles the absent ROM gracefully.

**Must never ship:**
- `doubletalkpc.bin` (proprietary RC Systems firmware; company still active).
- **The current `nvda/doubletalkpc.nvda-addon` as built** — it bundles the ROM.
  Rebuild without the ROM before any public release.

**Before either release, do this housekeeping:**
- MAME PR: fill in the empty `copyright-holders:` lines (Christopher Toth,
  David Sexton), convert `logerror` tracing to `LOGMASKED`/`VERBOSE 0`, run
  `srcclean`, split into CPU-core PR then ISA-card PR, disclose AI assistance.
- Distribution: add `copyright-holders`/BSD-3 headers to our own files that lack
  them, ship a BSD-3 NOTICE listing the MAME authors, and change
  `build_addon.sh` to exclude the ROM.
