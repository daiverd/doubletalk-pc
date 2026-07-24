# DoubleTalk PC — standalone emulator

A software emulator for the **RC Systems DoubleTalk PC**, an early-1990s 8-bit
ISA text-to-speech card. It runs the card's original 80C188EB firmware on a
vendored CPU core, so the *firmware itself* does all the speech synthesis — you
feed it text bytes and it produces the same PCM audio a 1993 screen reader would
have heard, no ISA slot or physical card required.

The card's firmware ROM is proprietary and is **not included** — you supply your
own (see [The firmware ROM](#the-firmware-rom)).

## How it works

The DoubleTalk PC is essentially a tiny embedded computer: an Intel 80C188EB CPU
with 512 KB of firmware that turns ASCII text into speech and streams it out an
8-bit DAC. This project emulates that CPU and its on-board peripherals (timers,
interrupt controller, the host mailbox) accurately enough that the **unmodified
firmware boots and synthesizes speech**. Nothing here reimplements the speech
algorithms — they live in the ROM and are executed as-is.

The CPU core is vendored verbatim from [MAME](https://github.com/mamedev/mame)
and compiled against a small compatibility shim so it builds and runs outside
MAME. See [`notes/`](notes/) for the full reverse-engineering write-up.

## Repository layout

| Path | What |
|---|---|
| `doubletalk/mame/` | Vendored MAME 8086/80186 + **80C188EB** CPU core (BSD-3-Clause) |
| `doubletalk/shim/` | Minimal MAME-compatibility shim so the core builds standalone |
| `doubletalk/doubletalk_board.{cpp,h}` | Board wrapper: CPU + I/O ports + DAC + host protocol |
| `doubletalk/dtalk.{cpp,h}` | C API over the board |
| `doubletalk/dtalk_cli.cpp` | Command-line harness (boot trace, synthesize to WAV) |
| `doubletalk/nvda/` | NVDA screen-reader add-on ([README](doubletalk/nvda/README.md)) |
| `driver/` | Linux kernel DoubleTalk drivers — **GPL-2.0**, included only as protocol reference |
| `notes/` | Reverse-engineering documentation |
| `LICENSE`, `LICENSING.md` | License text and per-component licensing/attribution map |

## Building

Requires a C++20 compiler (`g++`) and `make`.

```sh
cd doubletalk
make                 # builds build/dtalk_cli and build/libdtalk.a
```

Windows DLLs for the NVDA add-on (via mingw-w64 cross-compilers):

```sh
make windows         # needs g++-mingw-w64-i686 and g++-mingw-w64-x86-64
```

## Command-line usage

`dtalk_cli` takes the path to your ROM dump as its first argument:

```sh
# Synthesize text to an 8-bit WAV
./build/dtalk_cli doubletalkpc.bin say "Hello, world" hello.wav

# 16-bit WAV through the modeled DAC output stage (optional low-pass corner in Hz)
./build/dtalk_cli doubletalkpc.bin say16 "Hello, world" hello.wav 3000

# Boot the firmware and report state / instruction trace (diagnostics)
./build/dtalk_cli doubletalkpc.bin boot
./build/dtalk_cli doubletalkpc.bin trace 100000
```

Run `dtalk_cli` with no arguments to see the full command list.

## NVDA add-on

An [NVDA](https://www.nvaccess.org/) speech-synthesizer driver lives in
[`doubletalk/nvda/`](doubletalk/nvda/). It maps NVDA's rate/pitch/volume/voice
controls onto the card's own commands and plays the firmware's 10.5 kHz PCM.

> **Status:** working on NVDA (2023.1+, Windows).

See the [add-on README](doubletalk/nvda/README.md) for build and install steps.

## The firmware ROM

`doubletalkpc.bin` (512 KB) is proprietary RC Systems firmware. It carries no
redistribution grant and is **not included here or in any release**. Following
standard MAME practice, this project identifies the ROM by hash but gives no
sourcing guidance — supply your own dump (e.g. read it from a DoubleTalk PC card
you own) and verify it:

- **CRC32:** `66685631`
- **SHA1:** `bf7e78d6381c76d291ee069971873347a314ffff`

The repository `.gitignore` blocks `*.bin` / `rom/` so the ROM can't be
committed by accident.

## Licensing

The code **we** wrote is **BSD-3-Clause** — free to use, modify, and
redistribute (including commercially); just keep the copyright notice and don't
use the authors' names to endorse derived products. It does **not** require you
to open-source the rest of your project.

The repository also contains third-party components under their own licenses
(the vendored MAME core is BSD-3-Clause; the `driver/` Linux drivers are GPL-2.0
and are not linked into the emulator). See **[LICENSING.md](LICENSING.md)** for a
complete, per-file breakdown of licenses and copyright holders.

## Credits

- **David Sexton** — standalone port, shim, board wrapper, C API/CLI, NVDA add-on.
- **Christopher Toth** — the 80C188EB CPU support in the MAME core and the
  firmware-to-audio reverse engineering (see
  [`notes/investigation-audio-path.md`](notes/investigation-audio-path.md)).
- **MAME** and its contributors — the 8086/80186 CPU core this builds on.
