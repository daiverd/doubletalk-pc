# license: BSD-3-Clause
# copyright-holders: David Sexton
# (NVDA's GPL-2 license includes an explicit exception permitting non-GPL
# drivers and plugins; BSD-3 is additionally GPL-compatible regardless.)
#
# "DoubleTalk PC dictionaries" in NVDA's Settings dialog: which pronunciation
# dictionary files load, in what order, and a way to reload them without
# restarting the synthesizer.
#
# Why a global plugin and not the synth driver. A driver module is loaded when
# its synthesizer is selected and unloaded when it is not, so a settings
# category registered there would appear and disappear with the synthesizer -
# and worse, could be pulled out from under an open Settings dialog. Global
# plugins live for the whole session, which is what a settings category needs.
#
# What the panel is actually for. rcdict matches first-match-wins in load order,
# so a rule in a later file cannot override an earlier one - it can only add.
# The single thing a user needs control of, then, is the ORDER, and there is no
# way to express it in a folder of files except by naming them so they sort the
# way you want. Hence a list with Move up / Move down, and hence the order
# living in NVDA's configuration rather than in the filenames.
#
# Add does not copy. It records where the file is and leaves it there, so the
# file the user edits is the file the synthesizer reads - press Reload and the
# edit is live. Copying would have made every dictionary two files, one of them
# quietly stale, and "why did my change do nothing" the commonest question about
# this panel. Remove is the mirror of that: a reference is dropped from the
# list, and only a file in the add-on's own folder is deleted, because that
# folder is scanned and unlisting a file in it would not stick.
#
# Everything here is keyboard-first: the buttons keep the focus when they act
# (so Move up can be pressed four times without chasing the selection around),
# and anything that happens without focus moving is spoken with ui.message,
# because a wx selection changed in code raises no event for NVDA to announce.

import os

import wx

import globalPluginHandler
import globalVars
import gui
import synthDriverHandler
import ui
from gui import guiHelper
from gui.settingsDialogs import NVDASettingsDialog, SettingsPanel
from logHandler import log

# The driver package owns the rule about which files load and in what order,
# because the driver has to work whether or not this plugin does - and never
# the other way round. Caught rather than allowed to propagate so that a
# half-installed add-on costs a log line and this one category, instead of
# taking the whole global plugin down with a traceback at import time.
try:
	from synthDrivers.doubletalkpc import dictfiles
except ImportError:
	dictfiles = None

#: The synthesizer these dictionaries belong to (SynthDriver.name).
SYNTH_NAME = "doubletalkpc"

#: What NVDA's own settings panels wrap their descriptions at.
PANEL_DESCRIPTION_WIDTH = 544


def _count(n, singular, plural):
	"""'1 file' / '2 files', because "1 files" reads as a bug."""
	return "%d %s" % (n, singular if n == 1 else plural)


class DictionariesPanel(SettingsPanel):
	# The name of this category in NVDA's Settings dialog.
	title = "DoubleTalk PC dictionaries"

	panelDescription = (
		"Pronunciation dictionaries for the DoubleTalk PC (emulated) "
		"synthesizer. Rules are tried from the top of the list downwards and "
		"the first match wins, so a file higher in the list overrides one "
		"below it. Files are read where they are, so you can edit one and "
		"press Reload dictionaries."
	)

	def makeSettings(self, settingsSizer):
		sHelper = guiHelper.BoxSizerHelper(self, sizer=settingsSizer)

		# The same sentence as panelDescription, which is only ever an
		# accessible description; this is the one a sighted user reads.
		description = sHelper.addItem(
			wx.StaticText(self, label=self.panelDescription))
		description.Wrap(self.scaleSize(PANEL_DESCRIPTION_WIDTH))

		#: The working order, as dictfiles entries. Held here rather than read
		#: back out of the list control so that a move is a list operation and
		#: not a screen-scrape - and because what the list SHOWS is a label,
		#: which is not what goes in the configuration.
		self._entries = dictfiles.orderedEntries()

		self._listBox = sHelper.addLabeledControl(
			"Dictionary &files, in the order they are loaded",
			wx.ListBox,
			choices=self._labels(),
		)
		self._listBox.Bind(wx.EVT_LISTBOX, self.onSelectionChanged)

		bHelper = guiHelper.ButtonHelper(wx.HORIZONTAL)
		# Mnemonics chosen to clear the dialog's own OK / Cancel / Apply, which
		# take O, C and A.
		self._addButton = bHelper.addButton(self, label="A&dd...")
		self._removeButton = bHelper.addButton(self, label="&Remove")
		self._upButton = bHelper.addButton(self, label="Move &up")
		self._downButton = bHelper.addButton(self, label="Move dow&n")
		self._reloadButton = bHelper.addButton(self, label="Re&load dictionaries")
		sHelper.addItem(bHelper)

		self._addButton.Bind(wx.EVT_BUTTON, self.onAdd)
		self._removeButton.Bind(wx.EVT_BUTTON, self.onRemove)
		self._upButton.Bind(wx.EVT_BUTTON, lambda evt: self._move(-1))
		self._downButton.Bind(wx.EVT_BUTTON, lambda evt: self._move(1))
		self._reloadButton.Bind(wx.EVT_BUTTON, self.onReload)

		# Add can reach a file anywhere, so this is no longer where dictionaries
		# have to live - but it is still scanned, and it is still the answer for
		# someone who just wants somewhere to put one.
		sHelper.addItem(wx.StaticText(
			self,
			label="Files added here stay where they are. Anything placed in "
				"%s is loaded as well." % dictfiles.folderPath(),
		))

		self._selectIndex(0)

	def onPanelActivated(self):
		# The folder can have changed since the panel was built - a file dropped
		# in by hand, or an editor saving a new one - and the panel may well
		# have been built at the start of the session. Re-merge rather than
		# reload, so an order the user has arranged but not yet applied is not
		# thrown away by walking out of the category and back in. It also
		# refreshes the "not found" marks, which is the other thing that can
		# change under a panel left open.
		#
		# The selection is restored by ENTRY rather than by index, because the
		# merge can insert above it.
		index = self._listBox.GetSelection()
		current = self._entries[index] \
			if 0 <= index < len(self._entries) else None
		self._entries = dictfiles.mergeOrder(self._entries)
		self._rebuild(
			self._entries.index(current) if current in self._entries else 0)
		super(DictionariesPanel, self).onPanelActivated()

	# --- the list -------------------------------------------------------------

	def _labels(self):
		return [dictfiles.label(e) for e in self._entries]

	def _find(self, entry):
		"""Where C{entry} is in the working list, or -1.

		Same file, not same spelling, and dictfiles is the one that decides
		which is which - the panel must not grow a second opinion about it.
		"""
		wanted = dictfiles.key(dictfiles.normalize(entry))
		keys = [dictfiles.key(e) for e in self._entries]
		try:
			return keys.index(wanted)
		except ValueError:
			return -1

	def _rebuild(self, select=0):
		self._listBox.Set(self._labels())
		self._selectIndex(select)

	def _selectIndex(self, index):
		if self._entries:
			index = max(0, min(index, len(self._entries) - 1))
			self._listBox.SetSelection(index)
		self._updateButtons()

	def _updateButtons(self):
		index = self._listBox.GetSelection()
		has = index != wx.NOT_FOUND
		self._removeButton.Enable(has)
		self._upButton.Enable(has and index > 0)
		self._downButton.Enable(has and index < len(self._entries) - 1)

	def onSelectionChanged(self, evt):
		self._updateButtons()

	def _move(self, delta):
		index = self._listBox.GetSelection()
		if index == wx.NOT_FOUND:
			return
		target = index + delta
		if target < 0 or target >= len(self._entries):
			return
		self._entries[index], self._entries[target] = \
			self._entries[target], self._entries[index]
		self._rebuild(target)
		# Focus stays on the button so the move can be repeated, which means
		# nothing announces the new position by itself.
		ui.message("%s, %d of %d"
			% (dictfiles.label(self._entries[target]),
				target + 1, len(self._entries)))

	# --- adding and removing --------------------------------------------------

	def onAdd(self, evt):
		"""Point the list at files, wherever they are. Nothing is copied.

		The dialog opens in the dictionary folder when there is one, because
		that is where a user with no opinion about where to keep dictionaries
		will have put them - but it is only a starting point, and anywhere is
		as good.
		"""
		with wx.FileDialog(
			self,
			message="Select pronunciation dictionaries to add",
			defaultDir=dictfiles.folderPath()
				if os.path.isdir(dictfiles.folderPath()) else "",
			wildcard="Pronunciation dictionaries (*%s)|*%s|All files (*.*)|*.*"
				% (dictfiles.EXTENSION, dictfiles.EXTENSION),
			style=wx.FD_OPEN | wx.FD_FILE_MUST_EXIST | wx.FD_MULTIPLE,
		) as dialog:
			if dialog.ShowModal() != wx.ID_OK:
				return
			chosen = dialog.GetPaths()

		added = []
		already = []
		for source in chosen:
			entry = dictfiles.normalize(source)
			if not entry:
				continue
			if self._find(entry) != -1:
				# Already listed, under whatever spelling. Adding it again would
				# be a second entry for one file, and the second could never
				# match anything the first had not already matched.
				already.append(entry)
				continue
			self._entries.append(entry)
			added.append(entry)

		if added:
			self._rebuild(self._find(added[-1]))
			ui.message("Added %s"
				% _count(len(added), "dictionary", "dictionaries"))
		elif already:
			index = self._find(already[-1])
			self._rebuild(index)
			ui.message("%s is already in the list"
				% dictfiles.label(self._entries[index]))

	def onRemove(self, evt):
		"""Take the selected dictionary out of the list.

		For a file in the add-on's own folder that means deleting it, and there
		is no way round that: the folder is scanned for anything the saved order
		has not seen, so a file merely dropped from the list would be picked
		straight back up. For a file anywhere else it means exactly what it
		says, and nothing on disk is touched - which is why only the first case
		asks.
		"""
		index = self._listBox.GetSelection()
		if index == wx.NOT_FOUND:
			return
		entry = self._entries[index]
		name = dictfiles.label(entry)
		if dictfiles.inFolder(entry):
			if gui.messageBox(
				"Remove %s?\n\nThe file will be deleted from %s."
					% (entry, dictfiles.folderPath()),
				"Remove dictionary",
				wx.YES_NO | wx.NO_DEFAULT | wx.ICON_WARNING, self,
			) != wx.YES:
				return
			try:
				os.remove(dictfiles.fullPath(entry))
			except OSError:
				log.exception("doubletalkpc: could not delete %s" % entry)
				gui.messageBox("Could not delete %s." % entry,
					"Error", wx.OK | wx.ICON_ERROR, self)
				return
		del self._entries[index]
		self._rebuild(index)
		ui.message("Removed %s" % name)

	# --- applying -------------------------------------------------------------

	def onSave(self):
		dictfiles.setOrder(self._entries)
		self._reloadSynth()

	def onReload(self, evt):
		"""Re-read the files, so editing one does not mean restarting the synth.

		This is the button the whole no-copying arrangement is for: the file the
		user just saved in their editor is the file the synthesizer reads, so
		re-reading it is the entire round trip.

		The folder is re-scanned first, so this also picks up a file written
		since the panel was opened; and the order is saved first, because a user
		who presses this expects the arrangement in front of them to be the one
		that takes effect, and applying a different order from the one shown
		would be the worst of both.
		"""
		self._entries = dictfiles.mergeOrder(self._entries)
		self._rebuild(self._listBox.GetSelection())
		dictfiles.setOrder(self._entries)
		result = self._reloadSynth()
		if result is None:
			ui.message(
				"Dictionaries will be loaded when the DoubleTalk PC "
				"synthesizer is next started.")
			return
		rules, files = result
		ui.message("Loaded %s from %s"
			% (_count(rules, "rule", "rules"),
				_count(files, "file", "files")))

	def _reloadSynth(self):
		"""Ask the running synthesizer to re-read its dictionaries.

		Returns (rules, files) - (0, 0) is a perfectly good answer, meaning the
		folder is empty - or None if this synthesizer is not the one speaking,
		which is an ordinary thing for it not to be and no reason to complain:
		the files are read when it next starts.
		"""
		synth = synthDriverHandler.getSynth()
		if synth is None or getattr(synth, "name", None) != SYNTH_NAME:
			return None
		try:
			return synth.reloadDictionaries()
		except Exception:
			log.exception("doubletalkpc: reloading dictionaries failed")
			return None


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
	def __init__(self):
		super(GlobalPlugin, self).__init__()
		# On the secure desktop there is nothing to configure and nowhere to
		# save it, and a file picker there is a hole rather than a feature.
		if globalVars.appArgs.secure:
			return
		if dictfiles is None:
			log.error("doubletalkpc: the synth driver's dictfiles module is "
				"missing, so the dictionaries settings category is not "
				"available. Reinstall the add-on.")
			return
		dictfiles.initConfig()
		if DictionariesPanel not in NVDASettingsDialog.categoryClasses:
			NVDASettingsDialog.categoryClasses.append(DictionariesPanel)

	def terminate(self):
		try:
			NVDASettingsDialog.categoryClasses.remove(DictionariesPanel)
		except ValueError:
			pass
		super(GlobalPlugin, self).terminate()
