# license: BSD-3-Clause
# copyright-holders: David Sexton
#
# Which pronunciation dictionaries load, and in what order.
#
# Two halves of the add-on need the same answer: the synth driver, which loads
# the files, and the settings panel, which lets the user arrange them. It lives
# here rather than with the panel because the driver has to work whether or not
# the panel ever loads - never the other way round.
#
# The rule in one sentence: the dictionaries are the files listed in NVDA's
# configuration, in that order, wherever on disk they live, plus every .dict
# file in <nvda config>/doubletalkpc/ that the list has not already named,
# appended in sorted order.
#
# Two sources, because they answer two different questions. The folder is the
# zero-configuration one: drop a file in and it works, which is how dictionaries
# worked before there was a panel and how a user who never opens the panel goes
# on using them. The list is for files kept ANYWHERE ELSE - a dictionary in a
# Dropbox folder, one under version control, one being edited in a text editor
# two windows away. Those are REFERENCED WHERE THEY LIE, never copied here: a
# copy is a second file that goes stale the moment the first is edited, and the
# whole point of the reload button is that editing a dictionary is a matter of
# saving it.
#
# So an entry is one of two things, and which one it is is decided by whether it
# has a directory part:
#
#   "spanish.dict"                a file in the dictionary folder; stored by
#                                 name so that a portable copy of the NVDA
#                                 configuration keeps working on another machine
#   "D:\\dicts\\spanish.dict"     a reference to a file somewhere else; stored
#                                 absolute, because a relative path would be
#                                 relative to whatever NVDA's working directory
#                                 happens to be
#
# Everything else follows from rcdict's one hard rule: matching is
# first-match-wins in LOAD ORDER, so a later file cannot override an earlier one
# - it can only add. Order is therefore not a cosmetic preference, it is the
# only setting there is, which is why it is worth a config key and four buttons.

import os

import config
import globalVars
from logHandler import log

#: Section in NVDA's configuration, and the folder under <nvda config>. Same
#: name for both, on purpose: one thing to tell a user to look for.
CONFIG_SECTION = "doubletalkpc"
FOLDER_NAME = "doubletalkpc"

EXTENSION = ".dict"

#: string_list rather than a single delimited string because a Windows path can
#: contain almost anything, including whatever separator would have been picked.
_CONFIG_SPEC = {
	"dictionaries": "string_list(default=list())",
}


def initConfig():
	"""Declare our configuration section.

	Idempotent and safe to call from anywhere; both the driver and the settings
	panel call it before touching config, because whichever of them runs first
	cannot know that the other has.
	"""
	if CONFIG_SECTION not in config.conf.spec:
		config.conf.spec[CONFIG_SECTION] = _CONFIG_SPEC


def folderPath(create=False):
	"""The folder scanned for dictionaries, created on demand.

	Not created unless asked for: a user with no dictionaries should not find an
	empty folder in their configuration wondering what it wants from them.
	"""
	path = os.path.join(globalVars.appArgs.configPath, FOLDER_NAME)
	if create and not os.path.isdir(path):
		os.makedirs(path)
	return path


def folderNames():
	"""Every dictionary file in the folder, sorted, whatever the order says."""
	try:
		return sorted(
			f for f in os.listdir(folderPath())
			if f.lower().endswith(EXTENSION)
		)
	except OSError:
		# No folder yet is the normal case, not a problem to report.
		return []


# --- entries ------------------------------------------------------------------
#
# An entry is the string that goes in the configuration and comes back out of
# it: a bare name for a file in the folder, an absolute path for one anywhere
# else. This is the only section that understands that distinction; the load
# order below it, the driver and the settings panel all just pass entries
# around and ask here whenever they need to know something about one.


def _fold(path):
	"""One spelling of a path, for comparing two of them.

	normcase does the Windows part of this - it is what turns a forward slash
	someone typed into a backslash - and lower() is on top of it rather than
	left to normcase because this module is tested off Windows, where normcase
	is the identity, and a case rule that only holds on the target platform is
	a case rule nothing checks.
	"""
	return os.path.normcase(path).lower()


def normalize(entry):
	"""The canonical form of one entry, or "" for something unusable.

	A path that happens to point INTO the dictionary folder becomes a bare name,
	so that adding a file already in the folder cannot produce a second listing
	of the same file - and so that the entry survives the configuration being
	carried to a machine where the folder is somewhere else.
	"""
	entry = entry.strip().strip('"')
	if not entry:
		return ""
	if not os.path.dirname(entry):
		return entry
	path = os.path.abspath(entry)
	if _fold(os.path.dirname(path)) == _fold(os.path.abspath(folderPath())):
		return os.path.basename(path)
	return path


def inFolder(entry):
	"""Is this entry one of the folder's own files rather than a reference?"""
	return not os.path.dirname(entry)


def fullPath(entry):
	"""The file an entry names."""
	return os.path.join(folderPath(), entry) if inFolder(entry) else entry


def exists(entry):
	"""Is the file there? A reference is allowed not to be; see mergeOrder."""
	return os.path.isfile(fullPath(entry))


def key(entry):
	"""What makes two entries the same file, for de-duplication.

	Case-folded, because Windows filesystems are, and the configuration may hold
	whatever spelling the file was added under. Public because the settings
	panel has to ask the same question of the list it is holding, and two
	answers to "is this the same dictionary" would be one too many.
	"""
	return _fold(os.path.abspath(fullPath(entry)))


def label(entry):
	"""How one entry reads in the settings panel.

	A folder file is just its name; there is one folder and the panel says where
	it is. A reference has to carry its directory or two files called
	spanish.dict are indistinguishable - and where the file is IS the
	interesting part of a reference. A missing one says so rather than sitting
	in the list looking as though it were doing something.
	"""
	if inFolder(entry):
		name = entry
	else:
		name = "%s (%s)" % (os.path.basename(entry), os.path.dirname(entry))
	return name if exists(entry) else "%s - not found" % name


# --- load order ---------------------------------------------------------------


def mergeOrder(preferred):
	"""The dictionaries to load, ordered by C{preferred}, folder files appended.

	Entries in C{preferred} are normalized and de-duplicated, then every .dict
	file in the folder that none of them names is appended in sorted order. That
	last clause is what keeps dropping a file into the folder working.

	The two kinds of entry are treated differently when the file is not there,
	and deliberately:

	  - a folder entry that has gone drops out, because the folder is scanned
	    and is therefore the authority on what is in it: a file deleted from it
	    is gone, and keeping the name would be keeping a ghost.
	  - a reference that has gone is KEPT, because it names a file this add-on
	    does not control - on a memory stick not plugged in, a network share not
	    mounted yet, a file its editor is halfway through rewriting. Dropping it
	    would mean the user silently loses the reference for good the next time
	    the panel saves. It is marked "not found" in the list and skipped at load
	    time; removing it is the user's decision to make.

	The folder's spelling of a name wins over the configuration's, because the
	filesystem is the authority on that.
	"""
	folder = dict((key(n), n) for n in folderNames())
	ordered = []
	seen = set()
	for entry in preferred:
		entry = normalize(entry)
		if not entry:
			continue
		k = key(entry)
		if k in seen:
			continue
		if inFolder(entry):
			entry = folder.get(k)
			if entry is None:
				continue
		seen.add(k)
		ordered.append(entry)
	ordered.extend(sorted(
		name for k, name in folder.items() if k not in seen))
	return ordered


def orderedEntries():
	"""The dictionaries to load, in load order, as entries."""
	return mergeOrder(savedOrder())


def orderedPaths():
	"""The dictionary files to load, in load order, as full paths.

	Only the ones that are actually there: this is what the driver hands to
	rcdict, and a reference to a file that is not currently reachable is a thing
	to skip this time round, not an error.
	"""
	return [fullPath(e) for e in orderedEntries() if exists(e)]


def savedOrder():
	"""The order stored in the configuration. Empty if it has never been set."""
	initConfig()
	try:
		return list(config.conf[CONFIG_SECTION]["dictionaries"])
	except Exception:
		# A config written by some other version, or a section that somehow did
		# not take the spec. Falling back to the sorted folder is exactly the
		# behaviour this add-on had before the order was configurable.
		log.debugWarning("doubletalkpc: could not read the dictionary order",
			exc_info=True)
		return []


def setOrder(entries):
	"""Store the load order."""
	initConfig()
	config.conf[CONFIG_SECTION]["dictionaries"] = [
		e for e in (normalize(x) for x in entries) if e
	]
