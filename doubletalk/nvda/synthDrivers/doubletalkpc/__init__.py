# DoubleTalk PC (emulated) synth driver for NVDA.
#
# Drives dtalk.dll (the standalone DoubleTalk PC emulator from
# rusty_tts/native/retrochip/doubletalk - vendored MAME 80C188EB core plus
# the card's original firmware ROM) as an NVDA speech synthesizer.
#
# Files expected next to this __init__.py:
#   dtalk64.dll        - `make win64` (NVDA 2025.2+ is a 64-bit process)
#   dtalk.dll          - `make win32` (older 32-bit NVDA)
#   doubletalkpc.bin   - the 512KB firmware ROM (proprietary; fetch per
#                        rusty_tts/scripts/fetch_roms.sh, not distributed
#                        with the add-on)

import ctypes
import os
import re
import threading
import queue

import config
import nvwave
from autoSettingsUtils.driverSetting import DriverSetting
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
		SynthDriver.RateSetting(),
		SynthDriver.PitchSetting(),
		SynthDriver.VolumeSetting(),
		# Reconstruction-filter corner of the modeled output stage: the card's
		# datasheet-recommended 3kHz (authentic) vs a brighter 4.8kHz.
		DriverSetting("filter", "&Filter", availableInSettingsRing=True, defaultVal="3000"),
	)
	supportedCommands = {IndexCommand, PitchCommand}
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}

	_filters = {
		"3000": StringParameterInfo("3000", "Classic (3 kHz)"),
		"4800": StringParameterInfo("4800", "Wide (4.8 kHz)"),
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
		self._pitch = 50
		self._volume = 50
		self._voice = "0"
		self._filter = "3000"
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

	# --- settings ---

	def _get_voice(self):
		return self._voice

	def _set_voice(self, value):
		if value in self._voices:
			self._voice = value

	def _getAvailableVoices(self):
		return self._voices

	def _get_rate(self):
		return self._rate

	def _set_rate(self, value):
		self._rate = value

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

	# --- speech ---

	@staticmethod
	def _mapPitch(pct):
		# NVDA pitch 0-100 -> card nP 0-99 (both default in the middle: 50 -> 50).
		return int(round(pct * 99 / 100))

	@staticmethod
	def _map0to9(pct):
		# NVDA 0-100 -> card 0-9. 50 -> 5 (the card's default speed/volume),
		# 90+ -> 9. Plain rounding would give 4 at 50 (banker's rounding of 4.5),
		# missing the card default, so scale so 50 lands exactly on 5.
		return min(9, pct * 10 // 100)

	def _prefix(self, pitchOverride=None):
		# nS speed 0-9, nP pitch 0-99, nV volume 0-9, nO voice 0-7.
		# Voice (nO) MUST come first: the manual notes it loads a preset that
		# resets pitch/tone/etc., so a trailing parameter would clobber it.
		#
		# Each voice's preset gives it its distinct pitch/tone (that is what
		# makes the 8 voices actually sound different). NVDA's defaults (all 50)
		# are the card's own defaults, so at 50 we OMIT the matching absolute
		# command and let the preset show through; only a slider the user moved
		# off 50 emits an absolute override. pitchOverride forces an absolute nP
		# regardless (used for capital-letter pitch changes).
		parts = ["\x01%sO" % self._voice]
		if self._rate != 50:
			parts.append("\x01%dS" % self._map0to9(self._rate))
		if pitchOverride is not None:
			parts.append("\x01%dP" % self._mapPitch(pitchOverride))
		elif self._pitch != 50:
			parts.append("\x01%dP" % self._mapPitch(self._pitch))
		if self._volume != 50:
			parts.append("\x01%dV" % self._map0to9(self._volume))
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
