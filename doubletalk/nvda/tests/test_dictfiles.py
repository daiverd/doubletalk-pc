"""Exercise dictfiles.py outside NVDA, with the modules it imports stubbed.

The interesting behaviour is all in mergeOrder: what happens when the configured
order and the folder disagree. That is worth a test because every one of those
disagreements is a real situation - a file deleted by hand, a file dropped in by
hand, a config written under a different spelling on a case-insensitive
filesystem, a dictionary referenced on a drive that is not plugged in today.

The two kinds of entry - a name in the add-on's folder, a path to a file
anywhere else - behave differently on purpose, and the difference is what the
second half of this file is about.

usage: test_dictfiles.py DRIVER_DIR
"""
import os
import sys
import tempfile

# The driver package is imported from the tree itself, so do not scatter
# __pycache__ directories through it - they would end up in the add-on.
sys.dont_write_bytecode = True

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nvdastub  # noqa: E402

configPath = tempfile.mkdtemp()
nvda = nvdastub.install(configPath)

sys.path.insert(0, os.path.abspath(sys.argv[1]))
import dictfiles  # noqa: E402

failures = []


def check(name, got, want):
	if got != want:
		failures.append("%s\n  got  %r\n  want %r" % (name, got, want))
		print("FAIL %s" % name)
	else:
		print("ok   %s" % name)


def setOrder(entries):
	# Written straight into the stub config: the point here is dictfiles' own
	# merge, not NVDA's configobj validation.
	nvda.config.conf.setdefault(dictfiles.CONFIG_SECTION, {})["dictionaries"] = list(entries)


folder = dictfiles.folderPath(create=True)

#: Somewhere that is not the dictionary folder, standing in for the user's own
#: dictionaries - the ones this add-on must read where they lie.
elsewhere = tempfile.mkdtemp()


def touch(path):
	open(path, "w").close()
	return path


def inFolder(name):
	return touch(os.path.join(folder, name))


def outside(name, directory=None):
	return touch(os.path.join(directory or elsewhere, name))


# --- the folder, which works with no configuration at all ---------------------

# No configuration: sorted folder order, which is what the add-on did before the
# order was configurable, and what a user who never opens the settings panel
# goes on getting.
inFolder("50-shared.dict")
inFolder("00-mine.dict")
inFolder("notes.txt")
check("unconfigured: sorted, non-.dict ignored",
	dictfiles.orderedEntries(), ["00-mine.dict", "50-shared.dict"])

# A configured order wins, even against sorting. This is the whole point of the
# panel: rules are first-match-wins in load order, so the order IS the priority.
setOrder(["50-shared.dict", "00-mine.dict"])
check("configured order wins",
	dictfiles.orderedEntries(), ["50-shared.dict", "00-mine.dict"])

# A file dropped into the folder by hand is appended, not lost.
inFolder("zz-new.dict")
inFolder("aa-new.dict")
check("unlisted folder files appended in sorted order",
	dictfiles.orderedEntries(),
	["50-shared.dict", "00-mine.dict", "aa-new.dict", "zz-new.dict"])

# A folder file named in the configuration but no longer there drops out: the
# folder is scanned, so it is the authority on what is in it.
setOrder(["gone.dict", "00-mine.dict", "50-shared.dict"])
check("missing folder files drop out",
	dictfiles.orderedEntries(),
	["00-mine.dict", "50-shared.dict", "aa-new.dict", "zz-new.dict"])

# Case: the config may hold whatever spelling the file was added under, but the
# folder is the authority on the real name.
setOrder(["00-MINE.DICT", "50-Shared.Dict"])
check("case-insensitive match, folder spelling returned",
	dictfiles.orderedEntries()[:2], ["00-mine.dict", "50-shared.dict"])

# A duplicated entry collapses onto the first, which is the one that would have
# won anyway.
setOrder(["zz-new.dict", "zz-new.dict", "00-mine.dict"])
check("duplicates collapse",
	dictfiles.orderedEntries(),
	["zz-new.dict", "00-mine.dict", "50-shared.dict", "aa-new.dict"])

# mergeOrder is what the settings panel calls to re-sync a list it is holding
# without discarding an arrangement the user has not applied yet.
check("mergeOrder keeps a working order and appends the rest",
	dictfiles.mergeOrder(["aa-new.dict"]),
	["aa-new.dict", "00-mine.dict", "50-shared.dict", "zz-new.dict"])


# --- references to files kept anywhere else -----------------------------------

# The reason for all of this: a dictionary the user keeps in their own folder is
# read where it lies, so editing it and pressing Reload is the whole workflow.
mine = outside("mine.dict")
setOrder([mine, "00-mine.dict"])
check("a path outside the folder is kept as a path",
	dictfiles.orderedEntries()[:2], [mine, "00-mine.dict"])
check("and it is what gets loaded",
	dictfiles.orderedPaths()[0], mine)

# Order across the two kinds is one order: a reference can outrank a folder file
# or sit below it, because first-match-wins does not care where a file lives.
setOrder(["00-mine.dict", mine])
check("references and folder files share one order",
	dictfiles.orderedEntries()[:2], ["00-mine.dict", mine])

# A reference to a file that is not there today is KEPT - the drive may simply
# not be plugged in - but it is not handed to rcdict.
missing = os.path.join(elsewhere, "not-here.dict")
setOrder([missing, "00-mine.dict"])
check("a missing reference stays in the list",
	dictfiles.orderedEntries()[0], missing)
check("a missing reference is not loaded",
	missing in dictfiles.orderedPaths(), False)
check("and the list says so",
	dictfiles.label(missing).endswith("not found"), True)

# A path that points into the dictionary folder is the same thing as the bare
# name, however it was written, so adding an already-installed file cannot
# produce two entries for one file.
setOrder([os.path.join(folder, "50-shared.dict"), "00-mine.dict"])
check("a path into the folder normalizes to a name",
	dictfiles.orderedEntries()[:2], ["50-shared.dict", "00-mine.dict"])
setOrder([os.path.join(folder, "50-shared.dict"), "50-shared.dict"])
check("the same file written two ways collapses",
	dictfiles.orderedEntries().count("50-shared.dict"), 1)

# Two files of the same name in different places are two dictionaries, and the
# list has to be able to hold both.
other = outside("mine.dict", tempfile.mkdtemp())
setOrder([mine, other])
check("same name, different directories, both kept",
	dictfiles.orderedEntries()[:2], [mine, other])
check("labels tell them apart",
	dictfiles.label(mine) != dictfiles.label(other), True)

# What goes back into the configuration is the normalized form, so the config
# does not accumulate the spellings of whatever file picker produced them.
dictfiles.setOrder([
	" %s " % os.path.join(folder, "00-mine.dict"), '"%s"' % mine, ""])
check("setOrder stores normalized entries",
	list(nvda.config.conf[dictfiles.CONFIG_SECTION]["dictionaries"]),
	["00-mine.dict", mine])

# A relative path is resolved against the working directory once, when it is
# stored, rather than meaning something different every time NVDA is started.
check("a relative path is stored absolute",
	dictfiles.normalize(os.path.join(".", "sub", "mine.dict")),
	os.path.abspath(os.path.join("sub", "mine.dict")))


# --- nothing at all -----------------------------------------------------------

# No folder: not an error, and the commonest state there is. A reference still
# works without one, which is the point of references.
nvda.globalVars.appArgs.configPath = tempfile.mkdtemp()
setOrder([])
check("no folder yet", dictfiles.orderedEntries(), [])
check("no folder yet, paths", dictfiles.orderedPaths(), [])
setOrder([mine])
check("a reference works with no folder at all",
	dictfiles.orderedPaths(), [mine])

print()
if failures:
	print("\n".join(failures))
	print("%d failures" % len(failures))
	sys.exit(1)
print("0 failures")
