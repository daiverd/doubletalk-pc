# DoubleTalk PC Speech Dispatcher module

`sd_doubletalk` is a native Speech Dispatcher output module for the standalone
DoubleTalk PC emulator in this repository. It links the existing
`../doubletalk/build/libdtalk.a` archive and streams the emulator's signed
16-bit mono PCM through Speech Dispatcher's server-side audio. By default it
resamples the emulator's native 10,504 Hz output to 48 kHz before handing it
off, avoiding awkward downstream resampling through PipeWire and network
tunnels.

The proprietary DoubleTalk firmware ROM is not included and must not be
redistributed. Supply your own 512 KiB `doubletalkpc.bin`.

## Prerequisites

- A C++20 compiler, GNU Make, and `pkg-config`
- Speech Dispatcher development headers and `libspeechd_module`
- SpeexDSP development headers and library
- An existing `doubletalk/build/libdtalk.a`

The module build reads from `../doubletalk/` but does not rebuild or modify it.

## Build and test

From the repository root:

```sh
make -C spd-module
make -C spd-module test
```

The executable is written to `spd-module/build/sd_doubletalk`. All module
objects and test binaries remain under `spd-module/build/`.

## Configure the ROM

Edit `spd-module/doubletalk.conf`:

```text
DoubleTalkRomPath "/absolute/path/to/doubletalkpc.bin"
DoubleTalkLowPassHz 3800
DoubleTalkOutputRate 48000
```

`DoubleTalkRomPath` is required and must name a readable file of exactly
524,288 bytes. `DoubleTalkLowPassHz` is optional; it accepts 500 through 5000
Hz and defaults to 3800 Hz, which the current emulator calibrates to the MAME
reference. The hardware datasheet's darker 3000 Hz analog corner remains
available.
`DoubleTalkOutputRate` is optional; it accepts 8000 through 192000 Hz and
defaults to 48000 Hz. Set it to 10504 to retain the emulator's native output
rate.

## Register the module

Add an entry using absolute paths to your Speech Dispatcher `speechd.conf`:

```text
AddModule "doubletalk" "/absolute/path/to/spd-module/build/sd_doubletalk" "/absolute/path/to/spd-module/doubletalk.conf"
```

For a per-user setup, the configuration is normally
`~/.config/speech-dispatcher/speechd.conf`. Restart Speech Dispatcher after
changing it. You can then confirm and exercise the module:

```sh
spd-say -O
spd-say -o doubletalk -L
spd-say -o doubletalk -w "Hello from DoubleTalk"
spd-say -o doubletalk -r 100 -p 20 -i 80 "Fast speech"
spd-say -o doubletalk -y "Vader" "Voice test"
```

## Supported controls

Speech Dispatcher rate, pitch, and volume values (`-100..100`) map to the
firmware's native speed (`0..9`), pitch (`0..99`), and volume (`0..9`)
commands. The module advertises all eight firmware voices:

- Perfect Paul
- Vader
- Big Bob
- Precise Pete
- Ricochet
- Biff
- Skip
- Robo Robert

Basic SSML tags are removed, standard XML entities are decoded, and
`<mark name="..."/>` elements are reported back as Speech Dispatcher index
marks. Each utterance selects firmware number mode `14B`, which preserves
leading zeros while continuing to pronounce ordinary numbers as numbers.
Stop is immediate. Pause stops the current utterance because the DoubleTalk
API does not expose resumable pause.

Language, punctuation mode, spelling mode, pitch range, capitalization mode,
and generic voice type are accepted for compatibility but do not change the
firmware output. Sound-icon requests speak the icon name.

## Troubleshooting

When the module cannot start, Speech Dispatcher logs the specific reason:
missing configuration, unreadable ROM, incorrect ROM size, or firmware boot
failure. To inspect module startup directly, its first protocol command must be
`INIT`; normal use should always go through Speech Dispatcher.
