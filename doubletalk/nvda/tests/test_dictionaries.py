"""Drive the real doubletalkpc NVDA driver against the real emulator.

Checks the things that are only true if the whole path is wired up: that a
dictionary is found and loaded, that speak() substitutes through it, that the
utterance still ends in the CR without which the card says nothing at all, and
that reloading picks up an edited file.

usage: test_dictionaries.py DRIVER_DIR SHARED_LIBRARY ROM

Run it through run.sh, which finds all three.
"""
import os
import shutil
import sys
import tempfile

# The driver package is imported from the tree itself, so do not scatter
# __pycache__ directories through it - they would end up in the add-on.
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nvdastub  # noqa: E402

driverDir, dylib, rom = sys.argv[1:4]

configPath = tempfile.mkdtemp()
dictFolder = os.path.join(configPath, "doubletalkpc")
os.makedirs(dictFolder)

nvda = nvdastub.install(configPath)

# A package the driver can be imported from, with the ROM and the native
# library beside it - the driver looks for both next to its __init__.py.
staging = tempfile.mkdtemp()
package = os.path.join(staging, "doubletalkpc")
shutil.copytree(driverDir, package)
shutil.copyfile(rom, os.path.join(package, "doubletalkpc.bin"))
shutil.copyfile(dylib, os.path.join(package, "dtalk64.dll"))
sys.path.insert(0, staging)

import doubletalkpc  # noqa: E402

failures = []


def check(name, condition, detail=""):
	if condition:
		print("ok   %s" % name)
	else:
		failures.append("%s %s" % (name, detail))
		print("FAIL %s %s" % (name, detail))


def quiesce(driver):
	"""Stop the synthesis thread so the test owns the queue.

	The driver starts a daemon thread that blocks on _queue.get(), so a test
	that inspects what speak() queued is racing it - and losing that race means
	the emulator renders the whole utterance, which for the split test below is
	minutes of speech nobody is listening to. A None on the queue is the
	driver's own way of telling the thread to return.
	"""
	driver._queue.put(None)
	driver._thread.join(timeout=5)
	assert not driver._thread.is_alive(), "synthesis thread did not stop"
	return driver


def render(driver, sequence):
	"""speak() and return the bytes it queued, without running the thread."""
	driver.speak(sequence)
	return driver._queue.get_nowait()


def writeDict(name, body):
	with open(os.path.join(dictFolder, name), "w") as f:
		f.write(body)


IndexCommand = nvda.commands.IndexCommand

# --- no dictionaries at all: the ordinary case, and nothing may change -------

driver = quiesce(doubletalkpc.SynthDriver())
plain = render(driver, ["I use NVDA."])
check("no dictionary: text passes through", b"I use NVDA." in plain, repr(plain))
check("no dictionary: utterance ends in CR", plain.endswith(b"\r"), repr(plain[-8:]))
check("no dictionary: nothing loaded", driver._dict is None)
driver.terminate()

# --- one dictionary ---------------------------------------------------------

writeDict("50-shared.dict", "#!rcdict 1\n#!case insensitive\nword\t\tNVDA\tenn vee dee ay\n")

driver = quiesce(doubletalkpc.SynthDriver())
check("dictionary loaded", driver._dict is not None)
spoken = render(driver, ["I use NVDA."])
check("substitution happens in speak()", b"enn vee dee ay" in spoken, repr(spoken))
check("original word is gone", b"NVDA" not in spoken, repr(spoken))
check("utterance still ends in CR", spoken.endswith(b"\r"), repr(spoken[-8:]))

# The prefix commands must survive expansion untouched: they are what select
# the voice, and rcdict is supposed to treat them as opaque atoms.
check("Ctrl-A prefix intact", spoken.startswith(b"\x01"), repr(spoken[:12]))
check("number mode intact", b"\x0114B" in spoken, repr(spoken))

# An index marker planted between two words must come through whole. A mangled
# \x01nI would change the card's mode or eat the next character.
withIndex = render(driver, ["I use ", IndexCommand(7), "NVDA."])
check("index marker survives", b"\x01%dI" % 0 in withIndex or b"\x011I" in withIndex,
	repr(withIndex))
check("substitution after a marker", b"enn vee dee ay" in withIndex, repr(withIndex))

# --- ordering: the earlier file wins ----------------------------------------

writeDict("00-mine.dict", "#!rcdict 1\n#!case insensitive\nword\t\tNVDA\tmy own version\n")

result = driver.reloadDictionaries()
check("reload reports counts", result is not None and result[1] == 2, repr(result))
spoken = render(driver, ["I use NVDA."])
check("first file in load order wins", b"my own version" in spoken, repr(spoken))

# --- phonemes make it through as a mode switch ------------------------------

writeDict("00-mine.dict",
	"#!rcdict 1\n#!case insensitive\nword\t\tNVDA\t[EH N V IY D IY EY]\n")
driver.reloadDictionaries()
spoken = render(driver, ["I use NVDA."])
check("phoneme span is wrapped in the mode switches",
	b"\x01D" in spoken and b"\x01T" in spoken, repr(spoken))

# --- the audio actually differs ---------------------------------------------
#
# The strongest check available here: run the emulator and require the
# substituted utterance to sound different from the plain one. Without this
# everything above could pass with a dictionary the card never receives.

import ctypes  # noqa: E402


def samples(driver, sequence):
	driver.speak(sequence)
	utterance = driver._queue.get_nowait()
	lib, h = driver._dt.lib, driver._dt.handle
	lib.dtalk_queue(h, utterance, len(utterance))
	buf = ctypes.create_string_buffer(2048 * 2)
	buf16 = ctypes.cast(buf, ctypes.POINTER(ctypes.c_int16))
	out = bytearray()
	while True:
		n = lib.dtalk_synth16(h, buf16, 2048)
		if n == 0:
			break
		out.extend(buf.raw[:n * 2])
	return bytes(out)


withDict = samples(driver, ["I use NVDA."])
driver.terminate()

os.remove(os.path.join(dictFolder, "00-mine.dict"))
os.remove(os.path.join(dictFolder, "50-shared.dict"))
driver = quiesce(doubletalkpc.SynthDriver())
check("dictionaries gone after removing the files", driver._dict is None)
without = samples(driver, ["I use NVDA."])
driver.terminate()

check("the card really speaks something different",
	len(withDict) > 1000 and len(without) > 1000 and withDict != without,
	"%d vs %d samples" % (len(withDict) // 2, len(without) // 2))

print()
if failures:
	print("%d failures" % len(failures))
	sys.exit(1)
print("0 failures")
