# license: BSD-3-Clause
# copyright-holders: David Sexton
# (NVDA's GPL-2 license includes an explicit exception permitting non-GPL
# synthesizer drivers; BSD-3 is additionally GPL-compatible regardless.)
#
# DoubleTalk PC (emulated) synth driver for NVDA.
#
# Drives dtalk.dll (the standalone DoubleTalk PC emulator from
# doubletalk-pc's doubletalk/ - vendored MAME 80C188EB core plus
# the card's original firmware ROM) as an NVDA speech synthesizer.
#
# Files expected next to this __init__.py:
#   dtalk64.dll        - `make win64` (NVDA 2025.2+ is a 64-bit process)
#   dtalk.dll          - `make win32` (older 32-bit NVDA)
#   doubletalkpc.bin   - the 512KB firmware ROM (proprietary to RC Systems, not
#                        distributed with the add-on). Supply your own dump;
#                        verify CRC32 66685631 / SHA1
#                        bf7e78d6381c76d291ee069971873347a314ffff.

import ctypes
import os
import re
import threading
import queue

import config
import nvwave
from autoSettingsUtils.driverSetting import DriverSetting, NumericDriverSetting
from autoSettingsUtils.utils import StringParameterInfo
from synthDriverHandler import SynthDriver, VoiceInfo, synthIndexReached, synthDoneSpeaking
from speech.commands import IndexCommand, PitchCommand
from logHandler import log

_DIR = os.path.dirname(__file__)

SAMPLES_PER_CHUNK = 2048


class _DtalkIndexMark(ctypes.Structure):
	_fields_ = [("value", ctypes.c_uint8), ("sample_pos", ctypes.c_uint64)]


def _dllName():
	# NVDA 2025.2+ runs as a 64-bit process; earlier versions are 32-bit.
	return "dtalk64.dll" if ctypes.sizeof(ctypes.c_void_p) == 8 else "dtalk.dll"


class _DtalkDLL:
	def __init__(self):
		self.lib = ctypes.cdll.LoadLibrary(os.path.join(_DIR, _dllName()))
		self.lib.dtalk_create.restype = ctypes.c_void_p
		self.lib.dtalk_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
		self.lib.dtalk_destroy.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_reset.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_sample_rate.restype = ctypes.c_uint32
		self.lib.dtalk_sample_rate.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_queue.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
		self.lib.dtalk_stop.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_set_lowpass_hz.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
		# Rate boost: rescales the firmware's ROM speech-rate table (in the
		# emulator's private in-RAM ROM copy) so speech can run faster than the
		# nS-9 command ceiling, pitch-preserved. Level 0 = authentic.
		self.lib.dtalk_rate_boost_max.restype = ctypes.c_int
		self.lib.dtalk_rate_boost_max.argtypes = []
		self.lib.dtalk_set_rate_boost.argtypes = [ctypes.c_void_p, ctypes.c_int]
		self.lib.dtalk_get_rate_boost.restype = ctypes.c_int
		self.lib.dtalk_get_rate_boost.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_active.restype = ctypes.c_int
		self.lib.dtalk_active.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_synth.restype = ctypes.c_size_t
		self.lib.dtalk_synth.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
		# Signed 16-bit path through the modeled output stage (filtered, gained).
		self.lib.dtalk_synth16.restype = ctypes.c_size_t
		self.lib.dtalk_synth16.argtypes = [
			ctypes.c_void_p, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t]
		self.lib.dtalk_read_index_marks.restype = ctypes.c_size_t
		self.lib.dtalk_read_index_marks.argtypes = [
			ctypes.c_void_p, ctypes.POINTER(_DtalkIndexMark), ctypes.c_size_t]

		with open(os.path.join(_DIR, "doubletalkpc.bin"), "rb") as f:
			rom = f.read()
		self.handle = self.lib.dtalk_create(rom, len(rom))
		if not self.handle:
			raise RuntimeError("dtalk_create failed (bad ROM?)")
		self.sample_rate = self.lib.dtalk_sample_rate(self.handle)

	def close(self):
		if self.handle:
			self.lib.dtalk_destroy(self.handle)
			self.handle = None


class SynthDriver(SynthDriver):
	name = "doubletalkpc"
	description = "DoubleTalk PC (emulated)"

	supportedSettings = (
		SynthDriver.VoiceSetting(),
		# The card's nS speed is natively 0-9, so give the 0-100 slider a coarse
		# minStep=10: each arrow-key press moves one real card step (11 stops ->
		# card 0-9, top two both 9) instead of landing in dead zones where many
		# slider positions collapse to the same nS value. 50% stays the
		# omit-at-default midpoint (-> nS 5, the card default).
		SynthDriver.RateSetting(minStep=10),
		# Faster-than-9S speech for power screen-reader use. OFF = the card's
		# authentic nS 0-9 range (rate slider maps 0-100 -> nS 0-9). ON rescales
		# the firmware rate table so the same slider positions run faster
		# (~1.8x at the top), pitch unchanged. See _set_rateBoost.
		SynthDriver.RateBoostSetting(),
		SynthDriver.PitchSetting(),
		# nV volume is also 0-9; same coarse step so each press = one card step.
		SynthDriver.VolumeSetting(minStep=10),
		# Reconstruction-filter corner of the modeled output stage: the card's
		# datasheet-recommended 3kHz (authentic) vs a brighter 4.8kHz.
		DriverSetting("filter", "&Filter", availableInSettingsRing=True, defaultVal="3000"),
		# Voice-quality knobs from the DoubleTalk PC/LT manual (Table 8). All are
		# native 0-9 (except tone, 0-2), presented as 0-100 sliders with the same
		# coarse minStep=10 as rate/volume so each step = one card value. These six
		# are VOICE-LINKED: selecting a voice snaps them to that voice's firmware
		# preset so the sliders reflect what is actually heard (WYSIWYG), and each
		# is emitted in the utterance prefix only when the user moves it OFF the
		# current voice's preset (see _voicePresets, _set_voice, _prefix). The
		# defaultVal here is voice 0 (Perfect Paul)'s preset, the startup voice.
		# Tone (nX) is a 3-way (bass/normal/treble) so it is a combo, like Filter.
		DriverSetting("tone", "&Tone", availableInSettingsRing=True, defaultVal="1"),
		# nA articulation 0-9, default 5A. Low = slurred, high = choppy.
		NumericDriverSetting("articulation", "&Articulation",
			availableInSettingsRing=True, defaultVal=50, minStep=10),
		# nE expression (intonation) 0-9, default 5E. 0 = monotone, 9 = animated.
		NumericDriverSetting("expression", "&Expression",
			availableInSettingsRing=True, defaultVal=50, minStep=10),
		# nF formant frequency 0-9, default 5F. Shifts overall vocal-tract voicing.
		NumericDriverSetting("formant", "&Formant frequency",
			availableInSettingsRing=True, defaultVal=50, minStep=10),
		# nR reverb 0-9, default 0R (none). Higher = more reverb delay/effect.
		NumericDriverSetting("reverb", "Re&verb",
			availableInSettingsRing=True, defaultVal=0, minStep=10),
	)
	supportedCommands = {IndexCommand, PitchCommand}
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}

	_filters = {
		"3000": StringParameterInfo("3000", "Classic (3 kHz)"),
		"4800": StringParameterInfo("4800", "Wide (4.8 kHz)"),
	}

	# nX tone: bass (0X), normal (1X, default), treble (2X) - like a stereo's
	# bass/treble controls (manual Table 8; Tone section).
	_tones = {
		"0": StringParameterInfo("0", "Bass"),
		"1": StringParameterInfo("1", "Normal"),
		"2": StringParameterInfo("2", "Treble"),
	}

	# nO voice numbers 0-7, names per the DoubleTalk PC/LT manual (Table 1)
	_voices = {
		"0": VoiceInfo("0", "Perfect Paul"),
		"1": VoiceInfo("1", "Vader"),
		"2": VoiceInfo("2", "Big Bob"),
		"3": VoiceInfo("3", "Precise Pete"),
		"4": VoiceInfo("4", "Ricochet"),
		"5": VoiceInfo("5", "Biff"),
		"6": VoiceInfo("6", "Skip"),
		"7": VoiceInfo("7", "Robo Robert"),
	}

	# Firmware voice presets, keyed by nO voice id. CARD-NATIVE values in the
	# order (pitch 0-99, articulation 0-9, formant 0-9, tone 0-2, expression 0-9,
	# reverb 0-9). These are ROM constants: verified by write-testing the card's
	# settings block via the `settingsmap` diagnostic (dtalk_cli). Selecting nO on
	# the card loads these, which is what gives each voice its distinct character
	# (e.g. Vader's low pitch 30, Ricochet's reverb 6, Big Bob's bass tone). The
	# alias voices 5/6/7 share 0/1/2's preset, expanded explicitly here.
	_voicePresets = {
		"0": (50, 5, 5, 1, 5, 0),  # Perfect Paul
		"1": (30, 4, 5, 1, 7, 2),  # Vader
		"2": (40, 4, 1, 0, 6, 0),  # Big Bob
		"3": (60, 8, 6, 2, 4, 0),  # Precise Pete
		"4": (40, 5, 2, 1, 5, 6),  # Ricochet
		"5": (50, 5, 5, 1, 5, 0),  # Biff        = Perfect Paul
		"6": (30, 4, 5, 1, 7, 2),  # Skip        = Vader
		"7": (40, 4, 1, 0, 6, 0),  # Robo Robert = Big Bob
	}

	@classmethod
	def check(cls):
		return os.path.isfile(os.path.join(_DIR, _dllName())) \
			and os.path.isfile(os.path.join(_DIR, "doubletalkpc.bin"))

	def __init__(self):
		self._dt = _DtalkDLL()
		# Serializes EVERY ctypes call into the (non-thread-safe) dtalk handle
		# across the main thread (cancel/terminate) and the synth thread.
		self._libLock = threading.Lock()
		# dtalk_synth16 emits signed 16-bit mono PCM through the modeled output
		# stage (DC block + reconstruction low-pass + MAME's 0.5 headroom gain),
		# which is exactly the WAVE_FORMAT_PCM 16-bit layout WavePlayer wants
		# (16-bit PCM is signed little-endian), so feed its bytes straight
		# through. This matches the cleaner MAME reference rather than the raw
		# 8-bit DAC staircase.
		self._player = nvwave.WavePlayer(
			channels=1,
			samplesPerSec=int(self._dt.sample_rate),
			bitsPerSample=16,
			outputDevice=config.conf["audio"]["outputDevice"],
		)
		self._rate = 50
		self._rateBoost = False
		# The boost level applied when rateBoost is ON: the strongest the DLL
		# reports as verified-safe (pitch-stable, monotonic, no fault at any
		# nS 0-9). OFF sends level 0 (authentic table).
		self._boostOnLevel = int(self._dt.lib.dtalk_rate_boost_max())
		self._pitch = 50
		self._volume = 50
		self._voice = "0"
		self._filter = "3000"
		# True only while NVDA is restoring saved settings (see loadSettings). It
		# guards _set_voice: a config restore/init must NOT snap the six voice-
		# linked settings to the voice preset (that would clobber a user's saved
		# per-voice value); an interactive voice change (flag False) DOES snap.
		self._restoring = False
		# Voice-quality settings, stored as their 0-100 slider value (tone as its
		# card string). Initial values are voice 0 (Perfect Paul)'s firmware preset
		# converted to slider units (articulation/expression/formant card 5 -> 50,
		# reverb card 0 -> 0, tone card 1 -> "1"); at these values _prefix emits
		# nothing for voice 0 and its preset shows through. On an interactive voice
		# change _set_voice re-snaps all six to the new voice's preset.
		self._tone = "1"
		self._articulation = 50
		self._expression = 50
		self._formant = 50
		self._reverb = 0
		# marker number (0-99, rolling) -> NVDA index value
		self._markMap = {}
		self._nextMark = 0
		self._queue = queue.Queue()
		self._stopping = threading.Event()
		self._thread = threading.Thread(target=self._synthLoop, daemon=True)
		self._thread.start()

	def terminate(self):
		self.cancel()
		self._queue.put(None)
		self._thread.join(timeout=5)
		self._player.close()
		with self._libLock:
			self._dt.close()

	def loadSettings(self, onlyChanged=False):
		# NVDA restores saved settings by first changing the voice and THEN
		# restoring each other setting (SynthDriver.loadSettings applies "voice"
		# before the rest of supportedSettings). If _set_voice snapped the six
		# voice-linked settings to the firmware preset during this, it would
		# overwrite the user's saved per-voice values a moment before they are
		# restored - and worse, an unchanged setting is skipped on an onlyChanged
		# profile switch, so the snap could stick. Guard the whole restore so
		# _set_voice does NOT snap here; the saved values (applied by the base
		# loadSettings after the voice change) win. Interactive voice changes
		# happen outside loadSettings (settings ring / Voice Settings dialog ->
		# changeVoice), where _restoring is False, so those still snap.
		self._restoring = True
		try:
			super().loadSettings(onlyChanged)
		finally:
			self._restoring = False

	# --- settings ---

	def _get_voice(self):
		return self._voice

	def _set_voice(self, value):
		if value not in self._voices:
			return
		self._voice = value
		if self._restoring:
			# Config restore / init: honor the saved per-voice settings, which
			# the base loadSettings applies right after this. Do not snap.
			return
		# Interactive voice change: snap the six voice-linked settings to the new
		# voice's firmware preset so the sliders (which NVDA re-reads via the
		# getters after changeVoice, both in the Voice Settings dialog and the
		# settings ring) show exactly what the card will produce for this voice.
		# Convert each card-native preset value to its NVDA slider units; these
		# round-trip back to the same card value in _prefix, so an untouched voice
		# emits only the nO select and its preset shows through.
		pitch, articulation, formant, tone, expression, reverb = \
			self._voicePresets[value]
		self._pitch = self._cardPitchToNvda(pitch)
		self._articulation = articulation * 10
		self._formant = formant * 10
		self._tone = str(tone)
		self._expression = expression * 10
		self._reverb = reverb * 10

	def _getAvailableVoices(self):
		return self._voices

	def _get_rate(self):
		return self._rate

	def _set_rate(self, value):
		self._rate = value

	def _get_rateBoost(self):
		return self._rateBoost

	def _set_rateBoost(self, enable):
		# Boost is applied in the emulator (the ROM rate table is rescaled),
		# independent of the nS command the prefix emits - so the rate slider ->
		# nS 0-9 mapping is unchanged and stays monotonic; ON just makes every
		# nS value (including the preset speed used when the rate slider sits at
		# its default 50 and no nS is sent) render faster, pitch preserved.
		enable = bool(enable)
		if enable == self._rateBoost:
			return
		self._rateBoost = enable
		with self._libLock:
			self._dt.lib.dtalk_set_rate_boost(
				self._dt.handle, self._boostOnLevel if enable else 0)

	def _get_pitch(self):
		return self._pitch

	def _set_pitch(self, value):
		self._pitch = value

	def _get_volume(self):
		return self._volume

	def _set_volume(self, value):
		self._volume = value

	def _get_availableFilters(self):
		return self._filters

	def _get_filter(self):
		return self._filter

	def _set_filter(self, value):
		if value not in self._filters:
			return
		self._filter = value
		with self._libLock:
			self._dt.lib.dtalk_set_lowpass_hz(self._dt.handle, int(value))

	def _get_availableTones(self):
		return self._tones

	def _get_tone(self):
		return self._tone

	def _set_tone(self, value):
		if value in self._tones:
			self._tone = value

	def _get_articulation(self):
		return self._articulation

	def _set_articulation(self, value):
		self._articulation = value

	def _get_expression(self):
		return self._expression

	def _set_expression(self, value):
		self._expression = value

	def _get_formant(self):
		return self._formant

	def _set_formant(self, value):
		self._formant = value

	def _get_reverb(self):
		return self._reverb

	def _set_reverb(self, value):
		self._reverb = value

	# --- speech ---

	@staticmethod
	def _mapPitch(pct):
		# NVDA pitch 0-100 -> card nP 0-99 (both default in the middle: 50 -> 50).
		return int(round(pct * 99 / 100))

	@staticmethod
	def _cardPitchToNvda(p):
		# Card nP 0-99 -> NVDA pitch 0-100, the inverse of _mapPitch chosen so it
		# round-trips: _mapPitch(_cardPitchToNvda(p)) == p for every preset pitch
		# (30->30, 40->40, 50->51->50, 60->61->60). Used to snap the pitch slider
		# to a voice preset so it reflects the pitch the card will actually use.
		return int(round(p * 100 / 99))

	@staticmethod
	def _map0to9(pct):
		# NVDA 0-100 -> card 0-9. 50 -> 5 (the card's default speed/volume),
		# 90+ -> 9. Plain rounding would give 4 at 50 (banker's rounding of 4.5),
		# missing the card default, so scale so 50 lands exactly on 5.
		return min(9, pct * 10 // 100)

	def _prefix(self, pitchOverride=None):
		# nS speed 0-9, nP pitch 0-99, nV volume 0-9, nO voice 0-7.
		# Voice (nO) MUST come first: the manual notes it loads the voice preset
		# (resetting pitch/tone/etc. on the card), so every following parameter
		# overrides that preset - and a trailing parameter would be clobbered.
		#
		# WYSIWYG emission: nO already loads the voice's preset on the card, so a
		# per-voice setting left at its preset needs no command - the preset shows
		# through exactly. For each of the six voice-linked settings we therefore
		# emit its absolute command ONLY when the current setting's card value
		# differs from THIS voice's preset (an untouched voice emits nothing but
		# nO; a value the user moved off-preset is emitted absolutely, never
		# re-flattened to a global default). Rate/Volume are NOT voice-linked
		# (nS/nV are constant across voices) so they keep the global omit-at-50%
		# rule. pitchOverride forces an absolute nP (capital-letter pitch changes).
		presetPitch, presetArtic, presetFormant, presetTone, presetExpr, \
			presetReverb = self._voicePresets[self._voice]
		parts = ["\x01%sO" % self._voice]
		if self._rate != 50:
			parts.append("\x01%dS" % self._map0to9(self._rate))
		if pitchOverride is not None:
			parts.append("\x01%dP" % self._mapPitch(pitchOverride))
		elif self._mapPitch(self._pitch) != presetPitch:
			parts.append("\x01%dP" % self._mapPitch(self._pitch))
		if self._volume != 50:
			parts.append("\x01%dV" % self._map0to9(self._volume))
		# Voice-quality overrides (after nO, like S/P/V): emitted only when moved
		# off the current voice's preset so the preset shows through untouched.
		if int(self._tone) != presetTone:
			parts.append("\x01%sX" % self._tone)
		if self._map0to9(self._articulation) != presetArtic:
			parts.append("\x01%dA" % self._map0to9(self._articulation))
		if self._map0to9(self._expression) != presetExpr:
			parts.append("\x01%dE" % self._map0to9(self._expression))
		if self._map0to9(self._formant) != presetFormant:
			parts.append("\x01%dF" % self._map0to9(self._formant))
		if self._map0to9(self._reverb) != presetReverb:
			parts.append("\x01%dR" % self._map0to9(self._reverb))
		return "".join(parts)

	def speak(self, speechSequence):
		parts = [self._prefix()]
		for item in speechSequence:
			if isinstance(item, str):
				# printable ASCII only; the firmware treats control bytes
				# as commands. Ctrl-A itself cannot occur after this.
				parts.append(re.sub(r"[^\x20-\x7e]", " ", item))
			elif isinstance(item, IndexCommand):
				mark = self._nextMark
				self._nextMark = (self._nextMark + 1) % 100
				self._markMap[mark] = item.index
				parts.append("\x01%dI" % mark)
			elif isinstance(item, PitchCommand):
				# NVDA raises the pitch before a capital and resets after.
				# offset is in NVDA pitch units (0-100); 0 means "back to base".
				offset = item.offset
				if offset == 0:
					# Restore the base state. Re-send the prefix so the nO
					# reloads the voice preset (the only way to recover the
					# preset pitch when the base pitch is the default 50 and no
					# absolute nP was sent) and any non-default rate/pitch/
					# volume overrides are re-applied.
					parts.append(self._prefix())
				else:
					eff = min(100, max(0, self._pitch + offset))
					parts.append("\x01%dP" % self._mapPitch(eff))
		parts.append("\r")
		self._queue.put("".join(parts).encode("ascii", "replace"))

	def cancel(self):
		self._stopping.set()
		try:
			while True:
				self._queue.get_nowait()
		except queue.Empty:
			pass
		with self._libLock:
			self._dt.lib.dtalk_stop(self._dt.handle)
		self._markMap.clear()
		self._player.stop()

	def pause(self, switch):
		self._player.pause(switch)

	# --- synthesis thread ---

	def _synthLoop(self):
		lib = self._dt.lib
		h = self._dt.handle
		# 16-bit samples: 2 bytes each. A c_char buffer of 2*chunk bytes, cast to
		# int16* for the call; buf.raw[:n*2] is the little-endian PCM to feed.
		buf = ctypes.create_string_buffer(SAMPLES_PER_CHUNK * 2)
		buf16 = ctypes.cast(buf, ctypes.POINTER(ctypes.c_int16))
		marks = (_DtalkIndexMark * 16)()
		# Cumulative dtalk_synth() output position; matches the emulator's own
		# absolute sample counter (dtalk_stop does not reset it), so index
		# marker sample_pos values line up across utterances.
		samplesDone = 0
		while True:
			utterance = self._queue.get()
			if utterance is None:
				return
			try:
				self._stopping.clear()
				with self._libLock:
					lib.dtalk_queue(h, utterance, len(utterance))
				pending = []  # (sample_pos, nvda_index) not yet fed past
				while not self._stopping.is_set():
					with self._libLock:
						n = lib.dtalk_synth16(h, buf16, SAMPLES_PER_CHUNK)
						if n:
							nm = lib.dtalk_read_index_marks(h, marks, 16)
							newMarks = [(marks[i].value, marks[i].sample_pos)
								for i in range(nm)]
							raw = buf.raw[:n * 2]  # n samples * 2 bytes (16-bit)
					if n == 0:
						break
					for value, samplePos in newMarks:
						nvdaIndex = self._markMap.pop(value, None)
						if nvdaIndex is not None:
							pending.append((samplePos, nvdaIndex))
					# raw is already signed 16-bit LE mono PCM = WavePlayer's format
					chunkEnd = samplesDone + n
					fired = [m for m in pending if m[0] <= chunkEnd]
					pending = [m for m in pending if m[0] > chunkEnd]

					def onDone(fired=fired):
						for _, idx in fired:
							synthIndexReached.notify(synth=self, index=idx)

					self._player.feed(raw, onDone=onDone if fired else None)
					samplesDone = chunkEnd
				if not self._stopping.is_set():
					self._player.idle()
					synthDoneSpeaking.notify(synth=self)
			except Exception:
				log.error("doubletalkpc synth thread", exc_info=True)
