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

   The Makefile links `-static` so the result depends on nothing but
   `KERNEL32.dll` and `msvcrt.dll`. This matters if you build the DLLs by
   hand: Debian ships mingw-w64 in two thread models (`update-alternatives
   --config x86_64-w64-mingw32-g++`), and under the *posix* one
   `-static-libgcc -static-libstdc++` still leaves a dependency on
   `libwinpthread-1.dll`. NVDA users have no mingw runtime, so that shows up
   as the driver silently failing to load. Check with:

       x86_64-w64-mingw32-objdump -p dtalk64.dll | grep 'DLL Name'

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

## Pronunciation dictionaries

The driver applies `rcdict`, a pronunciation layer shared byte-for-byte with
another engine: respellings, phonemes through the card's own phoneme mode (Ctrl-A
D), or embedded commands, matched by word, substring or regular expression.
The format is documented in `../rcdict/rcdict.h`, with a worked example in
`../rcdict/example.dict`.

**[The dictionary format is documented in `DICTIONARY-GUIDE.md`](../rcdict/DICTIONARY-GUIDE.md)** - a self-contained guide written for users rather than for this repository, and the thing to hand to anyone who asks how to write one.

Dictionaries are `.dict` files. They are arranged from **NVDA menu >
Preferences > Settings > DoubleTalk PC dictionaries**, which has a list of the
files in load order and five buttons:

| Button | Key | What it does |
|---|---|---|
| Add... | `Alt+D` | Adds a `.dict` file to the list, wherever it is |
| Remove | `Alt+R` | Takes it out of the list |
| Move up | `Alt+U` | Raises its priority |
| Move down | `Alt+N` | Lowers it |
| Reload dictionaries | `Alt+L` | Re-reads the files, so editing one does not mean restarting the synthesizer |

`Alt+F` moves to the list itself.

**Files are read where they are.** Add records where a file is; it does not copy
it. So the dictionary you edit is the one the synthesizer reads: save it in your
editor, press Reload, and the change is live. Keep them wherever suits - a
folder of your own, a synced drive, a checkout under version control.

A reference to a file that is not there right now (an unplugged drive, a share
not mounted yet) stays in the list, marked *not found*, and is skipped until it
comes back. Removing it is your decision, not the add-on's.

**Order is the whole point of the list.** Rules are tried from the top down and
the first match wins, so a file higher in the list overrides one below it - a
later file can only *add*. The order lives in NVDA's configuration, not in the
filenames.

**There is also a folder, for anyone who just wants somewhere to put one.**
`%APPDATA%\nvda\doubletalkpc\` is scanned as well: a `.dict` file dropped in
there works with no configuration at all, appended after everything the list
names, in sorted order. It is how dictionaries worked before there was a panel
and it still works that way - the panel is how you take control of what beats
what. Its files show in the list under their bare names, and because the folder
is scanned rather than listed, Remove on one of them has to delete it; the panel
says so and asks first. Remove on anything else only unlists it.

The list is applied when the Settings dialog is closed with OK or Apply, or
immediately by the Reload button.

The substitution is made before the utterance is queued, never inside
`dtalk_say` - this driver queues its own bytes with `dtalk_queue`, so anything
applied further down would never fire for NVDA.

## Testing

    make -C ../..          # the tests build a shared libdtalk from build/*.o
    tests/run.sh

`tests/` stubs NVDA's API (`nvdastub.py`), stages a copy of the driver package
with the host's shared library standing in for `dtalk64.dll`, and drives the
real `speak()`. It covers the dictionary path: that files are found and loaded
in the right order, that a substitution reaches the queued bytes, that the
Ctrl-A prefix and any index markers come through untouched, that the utterance
still ends in the CR without which the card says nothing at all, and - the
check the others rest on - that the card really renders different audio with a
dictionary loaded than without. `test_dictfiles.py` covers the load-order rules
on their own, with no emulator involved.
