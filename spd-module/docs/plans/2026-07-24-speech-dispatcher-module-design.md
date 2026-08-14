# DoubleTalk Speech Dispatcher Module Design

## Goal

Build a native Speech Dispatcher output module for the DoubleTalk PC emulator.
All new source, tests, configuration, documentation, and build artifacts live
under the top-level `spd-module/` directory. The existing `doubletalk/`
directory remains unchanged, and the module links against
`doubletalk/build/libdtalk.a`.

## Architecture

The module is a native executable named `sd_doubletalk`. It implements the
output-module callbacks declared by Speech Dispatcher's
`spd_module_main.h` and links with `libspeechd_module`, which supplies the
module process entry point and SSIP plumbing.

The module uses Speech Dispatcher's server-side audio transport. It streams
signed 16-bit, mono PCM from `dtalk_synth16()` at the rate reported by
`dtalk_sample_rate()` (currently 10,504 Hz). This avoids direct dependencies on
PulseAudio, PipeWire, or ALSA and lets Speech Dispatcher own audio routing.

The proprietary 512 KiB DoubleTalk firmware is not bundled. Its absolute path
is supplied through the module configuration.

## Configuration

The module configuration file supports:

```text
DoubleTalkRomPath "/absolute/path/doubletalkpc.bin"
DoubleTalkLowPassHz 3000
```

`DoubleTalkRomPath` is required. `DoubleTalkLowPassHz` is optional and defaults
to 3000 Hz. Startup fails with a clear message if the configuration cannot be
read, the ROM path is missing, the file cannot be read, its size is not exactly
512 KiB, or `dtalk_create()` rejects it.

## Speech Settings

Speech Dispatcher settings map to native DoubleTalk firmware commands:

| Speech Dispatcher | DoubleTalk |
|---|---|
| Rate `-100..100` | Speed `0..9` (`Ctrl-A nS`) |
| Pitch `-100..100` | Pitch `0..99` (`Ctrl-A nP`) |
| Volume `-100..100` | Volume `0..9` (`Ctrl-A nV`) |
| Synthesis voice | Voice `0..7` (`Ctrl-A nO`) |

The module advertises the eight firmware voices: Perfect Paul, Vader, Big Bob,
Precise Pete, Ricochet, Biff, Skip, and Robo Robert. A request prefix selects
the voice first, then applies rate, pitch, and volume so voice preset loading
does not overwrite explicit settings.

Language, punctuation, spelling, pitch-range, voice-type, and capitalization
settings are accepted but do not alter synthesis. This keeps normal Speech
Dispatcher setting negotiation from failing while avoiding unsupported
behavior.

## Text and Index Marks

Input is converted to printable ASCII. Non-ASCII and control characters become
spaces so arbitrary input cannot be interpreted as firmware commands.

Basic SSML is handled locally: ordinary tags are removed, common XML entities
are decoded, and `<mark name="..."/>` elements are replaced with rolling
DoubleTalk numeric index commands. The numeric marker is mapped back to the
original mark name when `dtalk_read_index_marks()` reports it. Marker
collisions are avoided within one request, and input containing more markers
than the firmware's 100 marker values is rejected.

Text, character, key, and spelling requests use the same synthesis path.
Sound-icon requests speak the icon name.

## Synthesis and Control Flow

For a speech request the module:

1. Validates and converts the input.
2. Confirms the request with `module_speak_ok()`.
3. Reports the begin event.
4. Queues the setting prefix, converted text, marks, and trailing carriage
   return through `dtalk_queue()`.
5. Pulls PCM in bounded chunks with `dtalk_synth16()`.
6. Splits chunks at reached index positions when necessary, sends PCM through
   `module_tts_output_server()`, and reports index names in playback order.
7. Reports end when synthesis finishes.

Speech Dispatcher processes stop commands while server-side audio chunks are
being submitted. `module_stop()` sets a stop flag and calls `dtalk_stop()`.
The synthesis loop then reports a stop event instead of an end event. Pause is
implemented as stop because the emulator does not provide resumable pause.

## Build and Layout

The new directory contains:

```text
spd-module/
  Makefile
  README.md
  doubletalk.conf
  src/
  tests/
  build/
```

`make` builds `spd-module/build/sd_doubletalk` with a C++20 compiler, the
installed Speech Dispatcher headers, `libspeechd_module`, pthreads, and
`../doubletalk/build/libdtalk.a`. It does not invoke the DoubleTalk build or
write anywhere beneath `doubletalk/`.

The README documents prerequisites, the ROM requirement, build commands,
user-local and system configuration, the `AddModule` entry, and `spd-say`
smoke tests.

## Testing

Unit tests do not require the proprietary ROM. They cover:

- configuration parsing and validation;
- rate, pitch, and volume range mapping;
- voice-name mapping and prefix ordering;
- ASCII sanitization and XML entity decoding;
- SSML tag removal and index-marker translation;
- rejection of malformed configuration and excess markers.

An optional integration test accepts a user-supplied ROM path and drives the
compiled module over stdin/stdout. It checks module initialization, protocol
responses, audio events for a short utterance, and stop handling.
