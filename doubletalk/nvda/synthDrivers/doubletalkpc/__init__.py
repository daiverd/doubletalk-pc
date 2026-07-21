# DoubleTalk PC (emulated) synth driver for NVDA.
#
# Drives dtalk.dll (the standalone DoubleTalk PC emulator from
# rusty_tts/native/retrochip/doubletalk - vendored MAME 80C188EB core plus
# the card's original firmware ROM) as an NVDA speech synthesizer.
#
# Files expected next to this __init__.py:
#   dtalk.dll          - built with `make win32` (32-bit, NVDA is a 32-bit
#                        process)
#   doubletalkpc.bin   - the 512KB firmware ROM (proprietary; fetch per
#                        rusty_tts/scripts/fetch_roms.sh, not distributed
#                        with the add-on)

import ctypes
import os
import re
import threading
import queue

import nvwave
import synthDriverHandler
from synthDriverHandler import SynthDriver, VoiceInfo, synthIndexReached, synthDoneSpeaking
from speech.commands import IndexCommand
from logHandler import log

_DIR = os.path.dirname(__file__)

SAMPLES_PER_CHUNK = 2048


class _DtalkIndexMark(ctypes.Structure):
	_fields_ = [("value", ctypes.c_uint8), ("sample_pos", ctypes.c_uint64)]


class _DtalkDLL:
	def __init__(self):
		self.lib = ctypes.cdll.LoadLibrary(os.path.join(_DIR, "dtalk.dll"))
		self.lib.dtalk_create.restype = ctypes.c_void_p
		self.lib.dtalk_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
		self.lib.dtalk_destroy.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_reset.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_sample_rate.restype = ctypes.c_uint32
		self.lib.dtalk_sample_rate.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_queue.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
		self.lib.dtalk_stop.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_active.restype = ctypes.c_int
		self.lib.dtalk_active.argtypes = [ctypes.c_void_p]
		self.lib.dtalk_synth.restype = ctypes.c_size_t
		self.lib.dtalk_synth.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
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
	)
	supportedCommands = {IndexCommand}
	supportedNotifications = {synthIndexReached, synthDoneSpeaking}

	# nO voice numbers 0-7
	_voices = {
		"0": VoiceInfo("0", "Voice 0 (default)"),
		"1": VoiceInfo("1", "Voice 1"),
		"2": VoiceInfo("2", "Voice 2"),
		"3": VoiceInfo("3", "Voice 3"),
		"4": VoiceInfo("4", "Voice 4"),
		"5": VoiceInfo("5", "Voice 5"),
		"6": VoiceInfo("6", "Voice 6"),
		"7": VoiceInfo("7", "Voice 7"),
	}

	@classmethod
	def check(cls):
		return os.path.isfile(os.path.join(_DIR, "dtalk.dll")) \
			and os.path.isfile(os.path.join(_DIR, "doubletalkpc.bin"))

	def __init__(self):
		self._dt = _DtalkDLL()
		self._player = nvwave.WavePlayer(
			channels=1,
			samplesPerSec=int(self._dt.sample_rate),
			bitsPerSample=16,
			outputDevice=synthDriverHandler._audioOutputDevice
				if hasattr(synthDriverHandler, "_audioOutputDevice") else None,
		)
		self._rate = 50
		self._pitch = 50
		self._volume = 100
		self._voice = "0"
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

	# --- speech ---

	def _settingsPrefix(self):
		# nS speed 0-9, nP pitch 0-99, nV volume 0-9, nO voice 0-7
		s = int(round(self._rate * 9 / 100))
		p = int(round(self._pitch * 99 / 100))
		v = int(round(self._volume * 9 / 100))
		return "\x01%dS\x01%dP\x01%dV\x01%sO" % (s, p, v, self._voice)

	def speak(self, speechSequence):
		parts = [self._settingsPrefix()]
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
		parts.append("\r")
		self._queue.put("".join(parts).encode("ascii", "replace"))

	def cancel(self):
		self._stopping.set()
		try:
			while True:
				self._queue.get_nowait()
		except queue.Empty:
			pass
		self._dt.lib.dtalk_stop(self._dt.handle)
		self._markMap.clear()
		self._player.stop()

	def pause(self, switch):
		self._player.pause(switch)

	# --- synthesis thread ---

	def _synthLoop(self):
		lib = self._dt.lib
		h = self._dt.handle
		buf = ctypes.create_string_buffer(SAMPLES_PER_CHUNK)
		marks = (_DtalkIndexMark * 16)()
		while True:
			utterance = self._queue.get()
			if utterance is None:
				return
			self._stopping.clear()
			lib.dtalk_queue(h, utterance, len(utterance))
			samplesDone = 0
			pending = []  # (sample_pos, nvda_index) not yet fed past
			while not self._stopping.is_set():
				n = lib.dtalk_synth(h, buf, SAMPLES_PER_CHUNK)
				if n == 0:
					break
				nm = lib.dtalk_read_index_marks(h, marks, 16)
				for i in range(nm):
					nvdaIndex = self._markMap.pop(marks[i].value, None)
					if nvdaIndex is not None:
						pending.append((marks[i].sample_pos, nvdaIndex))
				# u8 -> s16 for WavePlayer
				pcm = bytes(n * 2)
				pcm = bytearray(pcm)
				raw = buf.raw[:n]
				for i, b in enumerate(raw):
					v = (b - 128) << 8
					pcm[2 * i] = v & 0xff
					pcm[2 * i + 1] = (v >> 8) & 0xff
				chunkEnd = samplesDone + n
				fired = [m for m in pending if m[0] <= chunkEnd]
				pending = [m for m in pending if m[0] > chunkEnd]

				def onDone(fired=fired):
					for _, idx in fired:
						synthIndexReached.notify(synth=self, index=idx)

				self._player.feed(bytes(pcm), onDone=onDone if fired else None)
				samplesDone = chunkEnd
			if not self._stopping.is_set():
				self._player.idle()
				synthDoneSpeaking.notify(synth=self)
