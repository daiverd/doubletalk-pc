"""Just enough of NVDA's API to load a synth driver outside it.

Only what the two drivers actually touch. The point is to run the real speak()
against the real emulator, so anything that would make the driver take a
different path is stubbed as thinly as possible.
"""
import sys
import types


def install(configPath):
	# The drivers import FreeLibrary to unmap the DLL on terminate. It only
	# exists on Windows; here the library is never unmapped, which is exactly
	# what the driver does when it decides unloading is unsafe.
	import _ctypes
	if not hasattr(_ctypes, "FreeLibrary"):
		_ctypes.FreeLibrary = lambda handle: None

	# The drivers encode dictionary paths with "mbcs" - the Windows ANSI code
	# page, which is what the C runtime's fopen will decode a char* path with.
	# There is no such codec off Windows, and without this the driver logs
	# "could not load <path>" for every dictionary and quietly loads none, which
	# is a property of the harness and not of the driver.
	import codecs
	import encodings.cp1252
	try:
		codecs.lookup("mbcs")
	except LookupError:
		codecs.register(
			lambda name: encodings.cp1252.getregentry() if name == "mbcs" else None)

	config = types.ModuleType("config")

	class _Conf(dict):
		def __init__(self):
			dict.__init__(self)
			self.spec = {}
			self.profiles = []

	config.conf = _Conf()
	config.conf["audio"] = {"outputDevice": None}
	sys.modules["config"] = config

	globalVars = types.ModuleType("globalVars")
	globalVars.appArgs = types.SimpleNamespace(configPath=configPath, secure=False)
	sys.modules["globalVars"] = globalVars

	logHandler = types.ModuleType("logHandler")

	class _Log:
		def __init__(self):
			self.lines = []

		def _record(self, level):
			def write(*args, **kwargs):
				self.lines.append("%s: %s" % (level, args[0] if args else ""))
			return write

		def __getattr__(self, name):
			return self._record(name)

	logHandler.log = _Log()
	sys.modules["logHandler"] = logHandler

	nvwave = types.ModuleType("nvwave")

	class WavePlayer:
		"""Records what was fed, so a test can look at the audio."""

		def __init__(self, **kwargs):
			self.fed = bytearray()

		def feed(self, data, onDone=None):
			self.fed.extend(data)
			if onDone:
				onDone()

		def stop(self):
			pass

		def idle(self):
			pass

		def close(self):
			pass

		def pause(self, switch):
			pass

	nvwave.WavePlayer = WavePlayer
	sys.modules["nvwave"] = nvwave

	class _Setting:
		def __init__(self, id=None, displayName=None, **kwargs):
			self.id = id
			self.displayName = displayName
			for key, value in kwargs.items():
				setattr(self, key, value)

	driverSetting = types.ModuleType("autoSettingsUtils.driverSetting")
	driverSetting.DriverSetting = _Setting
	driverSetting.NumericDriverSetting = _Setting
	driverSetting.BooleanDriverSetting = _Setting
	utils = types.ModuleType("autoSettingsUtils.utils")

	class StringParameterInfo:
		def __init__(self, id, displayName):
			self.id, self.displayName = id, displayName

	utils.StringParameterInfo = StringParameterInfo
	autoSettingsUtils = types.ModuleType("autoSettingsUtils")
	autoSettingsUtils.driverSetting = driverSetting
	autoSettingsUtils.utils = utils
	sys.modules["autoSettingsUtils"] = autoSettingsUtils
	sys.modules["autoSettingsUtils.driverSetting"] = driverSetting
	sys.modules["autoSettingsUtils.utils"] = utils

	sdh = types.ModuleType("synthDriverHandler")

	class SynthDriver(object):
		@staticmethod
		def VoiceSetting(**kwargs):
			return _Setting("voice", "Voice", **kwargs)

		@staticmethod
		def RateSetting(**kwargs):
			return _Setting("rate", "Rate", **kwargs)

		@staticmethod
		def RateBoostSetting(**kwargs):
			return _Setting("rateBoost", "Rate boost", **kwargs)

		@staticmethod
		def PitchSetting(**kwargs):
			return _Setting("pitch", "Pitch", **kwargs)

		@staticmethod
		def VolumeSetting(**kwargs):
			return _Setting("volume", "Volume", **kwargs)

		@staticmethod
		def InflectionSetting(**kwargs):
			return _Setting("inflection", "Inflection", **kwargs)

		def loadSettings(self, onlyChanged=False):
			pass

	class VoiceInfo(StringParameterInfo):
		def __init__(self, id, displayName, language=None):
			StringParameterInfo.__init__(self, id, displayName)
			self.language = language

	class _Notification:
		def __init__(self):
			self.notified = []

		def notify(self, **kwargs):
			self.notified.append(kwargs)

	sdh.SynthDriver = SynthDriver
	sdh.VoiceInfo = VoiceInfo
	sdh.synthIndexReached = _Notification()
	sdh.synthDoneSpeaking = _Notification()
	sys.modules["synthDriverHandler"] = sdh

	commands = types.ModuleType("speech.commands")

	class IndexCommand:
		def __init__(self, index):
			self.index = index

	class _Offset:
		def __init__(self, offset):
			self.offset = offset

	class PitchCommand(_Offset):
		pass

	class RateCommand(_Offset):
		pass

	class VolumeCommand(_Offset):
		pass

	commands.IndexCommand = IndexCommand
	commands.PitchCommand = PitchCommand
	commands.RateCommand = RateCommand
	commands.VolumeCommand = VolumeCommand
	speech = types.ModuleType("speech")
	speech.commands = commands
	sys.modules["speech"] = speech
	sys.modules["speech.commands"] = commands

	return types.SimpleNamespace(
		config=config, globalVars=globalVars, log=logHandler.log,
		commands=commands, synthDriverHandler=sdh)
