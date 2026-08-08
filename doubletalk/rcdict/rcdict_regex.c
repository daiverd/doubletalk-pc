/* SPDX-License-Identifier: BSD-3-Clause */
/* rcdict_regex --- see rcdict_regex.h.
 *
 * Wraps Remimu (https://github.com/wareya/Remimu), vendored here as remimu.h
 * and released by its author into the public domain under CC0.  It was chosen
 * over the alternatives for reasons that are mostly not about regex:
 *
 *   - CC0 vendors into a BSD-3 module, and into doubletalk-pc's BSD-3 tree,
 *     with no licence friction.  That ruled out most candidates on its own.
 *   - No heap allocation and no recursion, so memory use is statically known
 *     and a pathological pattern cannot exhaust the stack.  In a screen
 *     reader's synthesis thread that matters more than speed.
 *   - A separate parse step, so a rule compiles when the dictionary loads
 *     rather than once per utterance.
 *
 * tiny-regex-c has no capture groups at all; SubReg re-parses the pattern on
 * every match, which is the wrong shape for a hot path with many rules.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Ceiling on backtracking steps for one match attempt.  Remimu defaults this
   to 0, meaning unlimited, and unlimited is not an option here: it is a
   backtracking engine, so (a+)+b against a run of a's is exponential, and a
   dictionary is a file the user edits.  Measured: with the limit set that
   pattern gives up immediately against thirty a's; without it, it hangs.
   200k steps is far more than any sane pattern needs on an utterance-sized
   string, and costs well under a millisecond to burn through. */
#define REMIMU_ITERATION_LIMIT 200000

/* The default is puts(), which would print to stdout from inside a synthesis
   thread every time the limit above is reached.  The caller is told through
   the return value instead. */
#define REMIMU_LOG_ERROR(x) ((void) (x))

#include "remimu.h"
#include "rcdict_regex.h"

/* Enough for the patterns a pronunciation dictionary carries; a longer one is
   refused at load with the rest of its line. */
#define RCDICT_REGEX_MAX_TOKENS 256

struct rcdict_regex
{
  int16_t ntokens;
  RegexToken tokens[1];		/* trailing, sized to ntokens */
};

rcdict_regex *
rcdict_regex_compile (const char *pattern)
{
  RegexToken scratch[RCDICT_REGEX_MAX_TOKENS];
  int16_t n = RCDICT_REGEX_MAX_TOKENS;
  rcdict_regex *re;
  size_t bytes;

  if (!pattern)
    return NULL;
  if (regex_parse (pattern, scratch, &n, 0) != 0)
    return NULL;
  if (n < 1)
    return NULL;

  /* A RegexToken is 40 bytes, so keeping the 256-entry scratch for every rule
     would cost 10 KB each.  Copy out just what was used. */
  bytes = sizeof *re + ((size_t) n - 1) * sizeof (RegexToken);
  re = malloc (bytes);
  if (!re)
    return NULL;
  re->ntokens = n;
  memcpy (re->tokens, scratch, (size_t) n * sizeof (RegexToken));
  return re;
}

void
rcdict_regex_free (rcdict_regex *re)
{
  free (re);
}

long
rcdict_regex_match (const rcdict_regex *re, const char *text, size_t at,
		    size_t ncaps, size_t *cap_start, size_t *cap_len)
{
  int64_t pos[16], span[16];
  uint16_t slots;
  int64_t r;
  size_t i;

  if (!re || !text)
    return RCDICT_REGEX_NOMATCH;

  slots = (uint16_t) (ncaps > 16 ? 16 : ncaps);
  for (i = 0; i < slots; i++)
    pos[i] = span[i] = -1;

  r = regex_match (re->tokens, text, at, slots, slots ? pos : NULL,
		   slots ? span : NULL);

  if (r == -2)
    return RCDICT_REGEX_GAVEUP;	/* iteration limit, or out of stack slots */
  if (r < 0)
    return RCDICT_REGEX_NOMATCH;

  /* THE CORRECTION.  regex_match returns the END INDEX, not the match length
     its own header claims.  At at==0 the two are equal, which is exactly why
     the documentation has survived: every doc example matches at 0.  Verified
     against the engine -- /[0-9]+/ on "abc  123xy" returns 8 from both at=5
     and at=6, which is an end index and cannot be a length. */
  if ((size_t) r < at)
    return RCDICT_REGEX_NOMATCH;	/* cannot happen; do not underflow if it does */

  for (i = 0; i < ncaps; i++)
    {
      if (cap_start)
	cap_start[i] = RCDICT_REGEX_UNSET;
      if (cap_len)
	cap_len[i] = 0;
      if (i < slots && pos[i] >= 0)
	{
	  if (cap_start)
	    cap_start[i] = (size_t) pos[i];
	  if (cap_len)
	    cap_len[i] = span[i] > 0 ? (size_t) span[i] : 0;
	}
    }

  return (long) ((size_t) r - at);
}

const char *
rcdict_regex_engine (void)
{
  return "Remimu (https://github.com/wareya/Remimu), CC0 / public domain";
}
