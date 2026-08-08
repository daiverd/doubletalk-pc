/* SPDX-License-Identifier: BSD-3-Clause */
/* rcdict_regex --- the regex matcher, kept behind a door.
 *
 * Everything about the vendored engine stops here: remimu.h is included by
 * rcdict_regex.c and nowhere else, so its assumptions cannot leak into the
 * rest of the module and it can be swapped without touching rcdict.c.
 *
 * The interface deliberately differs from the engine's in two places, because
 * the engine's documentation is wrong about both and the corrections belong
 * somewhere they cannot be forgotten:
 *
 *   - It returns a match LENGTH here.  regex_match returns the end index,
 *     despite its header saying "Returns match length"; the two agree only
 *     when matching at offset 0, which is why the mistake survives testing.
 *   - Matching is ANCHORED at `at` and never searches forward.  That is what
 *     the engine does, it suits the caller (which tries every position itself,
 *     so that first-match-wins keeps meaning what the dictionary says), and it
 *     is worth saying out loud because "match" usually implies otherwise.
 *
 * `text` must be NUL-terminated, and the NUL must be the end of what may be
 * matched: the engine has no length argument, so a caller with a bounded
 * region has to hand over a copy.  For rcdict that is the point rather than a
 * nuisance -- the region is one run of speakable text, and `^`, `$` and `\b`
 * should see its edges and not the command bytes on either side.
 */

#ifndef RCDICT_REGEX_H
#define RCDICT_REGEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct rcdict_regex rcdict_regex;

/* Compile a pattern.  NULL if it will not parse.  Compilation happens once,
   when the dictionary loads, and never while speaking. */
rcdict_regex *rcdict_regex_compile (const char *pattern);
void rcdict_regex_free (rcdict_regex *re);

#define RCDICT_REGEX_NOMATCH (-1)
#define RCDICT_REGEX_GAVEUP  (-2)

/* Match at text[at], anchored.  Returns the match length, or one of the two
   negatives above.

   GAVEUP means the engine hit its iteration limit -- this is a backtracking
   engine, and a pattern like (a+)+b against a run of a's is exponential.  The
   limit is what keeps a bad dictionary entry from wedging a screen reader's
   synthesis thread, so callers must treat it as "no match, and say so" rather
   than as an impossible case.

   cap_start/cap_len receive up to ncaps captures, as offsets into text.  Slot
   0 is the whole match; slots 1..n are the parenthesised groups.  Unset
   captures come back with cap_start == RCDICT_REGEX_UNSET. */
#define RCDICT_REGEX_UNSET ((size_t) -1)

long rcdict_regex_match (const rcdict_regex *re, const char *text, size_t at,
			 size_t ncaps, size_t *cap_start, size_t *cap_len);

/* Name of the engine and its licence, for an about box or a NOTICE file. */
const char *rcdict_regex_engine (void);

#ifdef __cplusplus
}
#endif

#endif /* RCDICT_REGEX_H */
