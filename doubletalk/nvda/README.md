# DoubleTalk PC (emulated) — NVDA add-on

NVDA speech synthesizer driver for the standalone DoubleTalk PC emulator
(`dtalk.dll`, built from the parent directory). The card's real 80C188EB
firmware does all the speech; NVDA just feeds it text bytes and plays the
10504Hz PCM it produces — the same path a 1993 screen reader used, minus
the ISA slot.

**Status: working on NVDA (2023.1+, Windows).** Built against the documented
NVDA synth driver API (`WavePlayer.feed(onDone=...)`, `synthIndexReached`).

## Building the add-on

1. Cross-compile the DLLs (64-bit for NVDA 2025.2+, 32-bit for older):

       make -C .. windows        # needs g++-mingw-w64-i686 and g++-mingw-w64-x86-64

2. Copy the pieces into the driver directory:

       cp ../build/win32/dtalk.dll ../build/win64/dtalk64.dll synthDrivers/doubletalkpc/
       cp <rom>/doubletalkpc.bin  synthDrivers/doubletalkpc/

   The firmware ROM is proprietary to RC Systems and is not distributed here.
   Supply your own dump (e.g. read it from a DoubleTalk PC card you own). Verify
   it against CRC32 `66685631` / SHA1 `bf7e78d6381c76d291ee069971873347a314ffff`.
   See LICENSING.md in the repository root.

3. Zip it up (an `.nvda-addon` is just a zip with `manifest.ini` at the
   root):

       ./build_addon.sh             # public-safe: no ROM bundled
       ./build_addon.sh --with-rom  # personal build: bundles the ROM - never distribute

4. Install by opening the `.nvda-addon` file on the Windows machine, then
   select "DoubleTalk PC (emulated)" in NVDA's synthesizer dialog.

## What's mapped

| NVDA setting | DoubleTalk command | Range |
|---|---|---|
| Rate         | Ctrl-A nS (speed)  | 0–9   |
| Pitch        | Ctrl-A nP          | 0–99  |
| Volume       | Ctrl-A nV          | 0–9   |
| Voice        | Ctrl-A nO          | 0–7   |

Index commands become Ctrl-A nI markers (rolling 0–99 mapped back to NVDA's
index values); the emulator reports each marker with its exact output-sample
position, which the driver converts to `synthIndexReached` notifications as
playback passes it. Cancel writes the card's own Ctrl-X clear command.
