/* SPDX-License-Identifier: BSD-3-Clause */
/* rcdict --- portable pronunciation dictionaries for the RC Systems
 * text-to-speech engines.
 *
 * This module is deliberately free of any dependency on either emulator: it
 * takes a byte stream on its way to the chip and returns the byte stream that
 * should go instead, with pronunciations substituted.  It is linked by a GPL-3
 * host and by a BSD-3 one, which is why it is BSD-3-Clause and must stay that
 * way -- a GPL-3 file could never move in the other direction.
 *
 * Targets differ only in trivia.  RC8650 datasheet Table 5 is captioned
 * "DoubleTalk Phoneme Symbols", so the phoneme set, the Table 6 attribute
 * modifiers, the D/T/C mode commands, the CTRL+A command character and the nI
 * index markers are common to both; a profile carries the rest.
 *
 *     rcdict_options o;
 *     rcdict_options_init (&o, &rcdict_rc8650);
 *     o.inline_phonemes = 1;
 *
 *     size_t n = rcdict_expand (&o, text, len, NULL, 0, NULL, NULL);
 *     char *buf = malloc (n + 1);
 *     rcdict_expand (&o, text, len, buf, n + 1, NULL, NULL);
 *
 * The input is NOT plain text.  By the time a screen reader's driver calls
 * this, index markers and prosody commands are already embedded in it, and
 * rewriting across one of those would corrupt a command rather than merely
 * mispronounce a word.  Everything here is built around that: command
 * sequences are opaque atoms, a substitution never spans one, and the scanner
 * tracks the two things that can change how the stream is read -- the command
 * character itself, and Zap.
 *
 * --- the dictionary format ------------------------------------------------
 *
 * Line-oriented UTF-8.  Comment lines begin with ';', the convention the
 * RC8650 datasheet sets for its own exception dictionaries, and they must be
 * lines of their own -- so a ';' inside a pattern needs no escaping.
 *
 * COLUMNS ARE SEPARATED BY TABS:
 *
 *     type <TAB> flags <TAB> pattern <TAB> output
 *     type <TAB> pattern <TAB> output          (no flags column)
 *     type <TAB> pattern                       (and no output: a silent match)
 *
 *     #!rcdict 1
 *     #!case insensitive        ; the file's default; 'sensitive' is the other
 *     #!lang en
 *
 *     word <TAB>   <TAB> NVDA <TAB> [EH N V IY D IY EY]
 *     word <TAB>   <TAB> Sean <TAB> Shon
 *     word <TAB> C <TAB> US   <TAB> [Y UW EH S]
 *     text <TAB>   <TAB> Mbps <TAB> megabits per second
 *
 * A tab cannot appear inside a field, so a pattern may contain spaces, '='
 * and ';' with nothing to escape.  A trailing tab is an empty last column and
 * is significant; no other whitespace is.
 *
 * Matchers:  word  whole words only, bounded by non-alphanumerics
 *            text  any substring
 *
 * Flags:     i  ignore case (the default)
 *            c  match case exactly
 *            C  ignore the pattern's case, but match only where the TEXT is
 *               all capitals -- "US" and not "us" or "Us", however the
 *               pattern itself was typed.  ('c' with an all-capitals pattern
 *               reaches the same place; 'C' is for when the pattern's own
 *               case is not something you want to have to get right.)
 *
 * A LINE WITH NO TABS IN IT is read the other way round, because tabs are
 * invisible and editors, config dialogs and web forms turn them into spaces
 * without saying so.  A dictionary that has been through one of those still
 * loads:
 *
 *     type[:flags] pattern = output      e.g.   word:C  US = [Y UW EH S]
 *
 * There the pattern ends at an '=' with whitespace either side, and "\=" is a
 * literal one.  The two spellings mean exactly the same thing.
 *
 * The output is a template, and one field rather than two, so that text and
 * phonemes can be mixed in a single entry:
 *
 *            [ ... ]  phonemes: Table 5 symbols, Table 6 modifiers, pauses
 *            { ... }  a raw chip command, e.g. {9S}
 *            \0       the matched text; \1-\9 are groups, for regex later
 *            \[ \] \{ \} \\ \=   literals
 *
 * Anything else is ordinary text, which goes back through the chip's
 * letter-to-sound stage -- so a Sean/Shon entry is a respelling and needs no
 * phonemes at all.  An empty output makes the match silent.
 */

#ifndef RCDICT_H
#define RCDICT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* --- target profiles ------------------------------------------------------ */

typedef struct rcdict_profile
{
  /* The command lead-in as the chip powers up.  Both targets use CTRL+A; a
     stream can change it, and rcdict_expand follows that when it happens. */
  unsigned char cmd_char;

  /* Mode commands, written with the profile's own command character. */
  const char *enter_phoneme;	/* "\x01" "D" */
  const char *leave_phoneme;	/* "\x01" "T" */

  /* Table 5, uppercase, NULL-terminated.  Letter mnemonics only: the pause
     symbols are handled separately because they are punctuation. */
  const char *const *phonemes;

  /* Table 5's pause symbols, longest first.  The RC8650 has ' (short), the
     DoubleTalk PC's table does not list it. */
  const char *pauses;

  /* Table 6 attribute modifiers.  These attach to phonemes rather than being
     space-delimited -- the manual's own example is "-/D>/EH R", which is
     -, /, D, >, /, EH, space, R. */
  const char *modifiers;

  /* Longest utterance the target's input buffer will take, for callers that
     split.  rcdict does not split; it only reports what it produced. */
  size_t max_utterance;
} rcdict_profile;

extern const rcdict_profile rcdict_rc8650;
extern const rcdict_profile rcdict_doubletalk_pc;

/* --- diagnostics ---------------------------------------------------------- */

/* Called for anything the caller would want to know about but which is not
   fatal: an unparsable line, an unknown phoneme symbol, a span emitted where
   the chip will not give it the right intonation.  offset is a byte offset
   into whatever was being read -- the input for rcdict_expand, the source text
   for the loaders, which also name the file and line in the message.  msg is
   valid only for the duration of the call.

   Nothing reported here is ever dropped silently: a bad rule is skipped and
   the rest of the file still loads, and a bad phoneme span is spoken as text.
   A dictionary must not be able to make a screen reader go quiet. */
typedef void (*rcdict_report) (void *ctx, size_t offset, const char *msg);

/* --- dictionaries --------------------------------------------------------- */

typedef struct rcdict rcdict;

rcdict *rcdict_new (const rcdict_profile *p);
void rcdict_free (rcdict *d);
void rcdict_clear (rcdict *d);
size_t rcdict_rule_count (const rcdict *d);

/* Load rules.  Each returns how many rules were added; a line that does not
   parse is reported and skipped, so a typo costs one entry and not the file.

   Rules accumulate in load order across calls, and matching is first-match-
   wins in that order -- so a later file cannot override an earlier one, it can
   only add to it.  Load the most specific dictionary FIRST.  (This is the
   datasheet's own rule for the chip's exception dictionaries, where (RATING)
   must precede (RAT), and it applies here for the same reason.) */
int rcdict_add_text (rcdict *d, const char *src, size_t len,
		     const char *name, rcdict_report report, void *ctx);
int rcdict_add_file (rcdict *d, const char *path,
		     rcdict_report report, void *ctx);

/* Split a separator-delimited path list and load each file in turn.  A file
   that is not there is reported but is not an error: a search path naming
   locations the user has not created yet is normal.

   Deliberately mechanism and not policy -- WHERE to look is the host's
   business, and keeping it there is what stops this module needing anything
   beyond stdio.  Reloading is the host's business too, for the same reason:
   watch the files however your platform prefers, then rcdict_clear and load
   again. */
int rcdict_add_path_list (rcdict *d, const char *list, char sep,
			  rcdict_report report, void *ctx);

/* --- options -------------------------------------------------------------- */

typedef struct rcdict_options
{
  const rcdict_profile *profile;

  /* Rules to apply.  Borrowed, not owned, and may be NULL for none. */
  const rcdict *dict;

  /* Recognise an inline phoneme escape in ordinary text, so that a
     pronunciation can be written anywhere text can -- including NVDA's own
     speech dictionaries, whose replacements are printable ASCII and would
     otherwise have no way to reach phoneme mode.

     OFF BY DEFAULT, and it must stay that way: "[[" is wiki link syntax, and a
     screen reader user reading a wiki page would otherwise find their text
     silently eaten.  Turning it on is a considered choice, not a default. */
  int inline_phonemes;
  const char *open;		/* default "[[" */
  const char *close;		/* default "]]" */
} rcdict_options;

void rcdict_options_init (rcdict_options *o, const rcdict_profile *p);

/* --- expansion ------------------------------------------------------------ */

/* Rewrite in[0..inlen) into out, returning the number of bytes the full result
   needs, not counting the terminating NUL -- so a return value >= outcap means
   the output was truncated, and the call can be repeated with a bigger buffer.
   out may be NULL when outcap is 0, which is how you ask for the size.

   Where this must be called from, for a streaming caller: BEFORE any splitting
   into utterance-sized pieces.  A word can expand to forty characters of
   phonemes plus the mode switches, so a piece that fitted the chip's input
   buffer before expansion may not afterwards, and the overflow is dropped by
   the input ring without a word. */
size_t rcdict_expand (const rcdict_options *opt,
		      const char *in, size_t inlen,
		      char *out, size_t outcap,
		      rcdict_report report, void *ctx);

/* --- helpers, for tooling ------------------------------------------------- */

/* Is sym[0..len) a Table 5 phoneme for this target?  Case-insensitive. */
int rcdict_is_phoneme (const rcdict_profile *p, const char *sym, size_t len);

/* Check a phoneme string the way rcdict_expand would.  Returns 1 if every
   symbol in it is known, 0 otherwise, reporting each unknown one. */
int rcdict_check_phonemes (const rcdict_profile *p, const char *s, size_t len,
			   rcdict_report report, void *ctx);

const char *rcdict_version (void);

#ifdef __cplusplus
}
#endif

#endif /* RCDICT_H */
