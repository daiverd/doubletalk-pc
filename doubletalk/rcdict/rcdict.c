/* SPDX-License-Identifier: BSD-3-Clause */
/* rcdict --- see rcdict.h.
 *
 * The substitution rules below are not guesses.  Each one is a measurement,
 * recorded in dict-lab/RESULTS.md and reproducible with dict-lab's scripts
 * against the letter-to-sound stage's own output buffer at 0x1036.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rcdict.h"
#include "rcdict_regex.h"

#define RCDICT_VERSION "0.2"

/* Long enough for any sane entry; a longer line is reported and skipped rather
   than silently truncated into something that means something else. */
#define RCDICT_MAX_LINE 4096

/* --- profiles ------------------------------------------------------------- */

/* Table 5, letter mnemonics.  55 of them, and the calibration in
   dict-lab/phoneme-codes.txt shows they land on 42 internal codes: nine groups
   share one (AH/AX, I/IY, EW/U/UW and friends), and five are compounds the
   chip expands itself (CH -> T SH, J -> D ZH, RR -> DX R DX). */
static const char *const table5[] = {
  "AA", "AE", "AH", "AW", "AX", "AY", "CH", "DH", "DX", "EH", "EI", "ER",
  "EW", "EY", "IH", "IX", "IY", "KX", "NG", "NY", "OW", "OY", "PX", "RR",
  "SH", "TH", "TX", "UH", "UW", "WH", "YY", "ZH",
  "A", "B", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
  "P", "R", "S", "T", "U", "V", "W", "Y", "Z",
  NULL
};

const rcdict_profile rcdict_rc8650 = {
  0x01,
  "\x01" "D",
  "\x01" "T",
  table5,
  ".,'",			/* the RC8650's table adds ' (short pause) */
  "/\\+-><",
  1900,				/* what the NVDA driver already splits at */
};

const rcdict_profile rcdict_doubletalk_pc = {
  0x01,
  "\x01" "D",
  "\x01" "T",
  table5,
  ".,",				/* the PC manual's Table 5 has no ' */
  "/\\+-><",
  1900,
};

void
rcdict_options_init (rcdict_options *o, const rcdict_profile *p)
{
  if (!o)
    return;
  o->profile = p ? p : &rcdict_rc8650;
  o->dict = NULL;
  o->inline_phonemes = 0;
  o->open = "[[";
  o->close = "]]";
}

const char *
rcdict_version (void)
{
  return RCDICT_VERSION;
}

/* --- small helpers -------------------------------------------------------- */

static int
is_space (int c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f';
}

static int
is_digit (int c)
{
  return c >= '0' && c <= '9';
}

static int
is_alpha (int c)
{
  return ((unsigned) c | 32) >= 'a' && ((unsigned) c | 32) <= 'z';
}

static int
upper (int c)
{
  return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static int
is_word_char (int c)
{
  return is_alpha (c) || is_digit (c);
}

int
rcdict_is_phoneme (const rcdict_profile *p, const char *sym, size_t len)
{
  size_t i;
  const char *const *t;
  if (!p || !sym || !len)
    return 0;
  for (t = p->phonemes; *t; t++)
    {
      if (strlen (*t) != len)
	continue;
      for (i = 0; i < len; i++)
	if (upper ((unsigned char) sym[i]) != (*t)[i])
	  break;
      if (i == len)
	return 1;
    }
  return 0;
}

/* --- output sink ---------------------------------------------------------- */

/* Writes what fits and counts what would have been needed, so one code path
   serves both the sizing call and the real one. */
typedef struct
{
  char *buf;
  size_t cap, len, need;
} sink;

/* Invariant, and what makes unput below correct: len is always
   min(need, cap-1).  need is the answer the caller wants; len is how much of it
   fitted. */
static void
put (sink * s, const char *p, size_t n)
{
  size_t room;
  s->need += n;
  if (!s->buf || s->len + 1 >= s->cap)
    return;
  room = s->cap - s->len - 1;	/* keep one for the NUL */
  if (n > room)
    n = room;
  memcpy (s->buf + s->len, p, n);
  s->len += n;
}

/* Take back the last n bytes.  Needed because whether the article before a
   span is absorbed is only known once the span is reached, by which time the
   article has been emitted as ordinary text.  Restoring the invariant is all
   that is required, so this works whether or not the buffer has already
   overflowed. */
static void
unput (sink * s, size_t n)
{
  if (n > s->need)
    n = s->need;
  s->need -= n;
  if (s->buf && s->cap)
    s->len = s->need < s->cap - 1 ? s->need : s->cap - 1;
}

static void
puts_ (sink * s, const char *p)
{
  put (s, p, strlen (p));
}

static void
putc_ (sink * s, char c)
{
  put (s, &c, 1);
}

static void
report_at (rcdict_report r, void *ctx, size_t off, const char *msg)
{
  if (r)
    r (ctx, off, msg);
}

/* --- phoneme strings ------------------------------------------------------ */

/* A phoneme string is not a list of space-delimited words.  Table 6's
   modifiers attach directly to phonemes -- the manual's own example is
   "70H AW   -/D>/EH R", where -/D>/EH is -, /, D, >, /, EH.  So it is scanned a
   character at a time: letter runs are mnemonics, digit runs are pitch values,
   and everything else is either a modifier, a pause or an error. */
int
rcdict_check_phonemes (const rcdict_profile *p, const char *s, size_t len,
		       rcdict_report report, void *ctx)
{
  size_t i = 0;
  int ok = 1;
  char msg[96];

  if (!p || !s)
    return 0;

  while (i < len)
    {
      unsigned char c = (unsigned char) s[i];

      if (is_space (c))
	{
	  i++;
	}
      else if (is_alpha (c))
	{
	  size_t start = i;
	  while (i < len && is_alpha ((unsigned char) s[i]))
	    i++;
	  if (!rcdict_is_phoneme (p, s + start, i - start))
	    {
	      size_t n = i - start;
	      char *q = msg;
	      if (n > 32)
		n = 32;
	      strcpy (q, "unknown phoneme symbol '");
	      q += strlen (q);
	      memcpy (q, s + start, n);
	      q += n;
	      *q++ = '\'';
	      *q = 0;
	      report_at (report, ctx, start, msg);
	      ok = 0;
	    }
	}
      else if (is_digit (c))
	{
	  /* Table 6: nn sets the pitch, 0-99. */
	  size_t start = i;
	  while (i < len && is_digit ((unsigned char) s[i]))
	    i++;
	  if (i - start > 2)
	    {
	      report_at (report, ctx, start,
			 "pitch value has more than two digits");
	      ok = 0;
	    }
	}
      else if (c && (strchr (p->modifiers, c) || strchr (p->pauses, c)))
	{
	  i++;
	}
      else
	{
	  char *q = msg;
	  strcpy (q, "not a phoneme, modifier or pause: '");
	  q += strlen (q);
	  *q++ = (char) c;
	  *q++ = '\'';
	  *q = 0;
	  report_at (report, ctx, i, msg);
	  ok = 0;
	  i++;
	}
    }
  return ok;
}

/* --- dictionaries --------------------------------------------------------- */

typedef enum
{
  SEG_TEXT,			/* literal, goes back through letter-to-sound */
  SEG_PHONEME,			/* [ ... ] */
  SEG_COMMAND,			/* { ... } */
  SEG_BACKREF			/* \0 - \9 */
} seg_kind;

typedef struct
{
  seg_kind kind;
  char *s;			/* owned; NULL for SEG_BACKREF */
  size_t len;
  int n;			/* backref number */
} seg;

/* Captures handed to the output template.  Slot 0 is the whole match, so \0
   works for every matcher and \1-\9 only ever mean something after a regex. */
#define RCDICT_NCAPS 10
typedef struct
{
  size_t start, len;
} rcap;

typedef enum
{
  M_WORD,
  M_TEXT,
  M_REGEX,
  M_RULE
} match_kind;

/* --- Table 22 context tokens ---------------------------------------------- */

/* The fifteen tokens, in the datasheet's own order, which is also the order of
   the class table below.  Nine of them are sets of characters and can be
   redefined per dictionary; the rest are structural and cannot. */
static const char rc_tokens[] = "#+^*:?@!%&$~\\|`";

#define RC_NTOK  15
#define RC_HASH  0		/* # a vowel                                  */
#define RC_PLUS  1		/* + a front vowel                            */
#define RC_HAT   2		/* ^ a consonant                              */
#define RC_STAR  3		/* * one or more consonants                   */
#define RC_COLON 4		/* : zero or more consonants                  */
#define RC_QUERY 5		/* ? a voiced consonant                       */
#define RC_AT    6		/* @ one of d j l n r s t z ch sh th          */
#define RC_BANG  7		/* ! one of b c d f g p t                     */
#define RC_PCT   8		/* % a suffix                                 */
#define RC_AMP   9		/* & a sibilant                               */
#define RC_DOLLAR 10		/* $ a nonalphabetic character                */
#define RC_TILDE 11		/* ~ one or more non-printing characters      */
#define RC_BSLASH 12		/* \ a digit                                  */
#define RC_BAR   13		/* | one or more digits, commas ignored       */
#define RC_WILD  14		/* ` any character                            */

/* Class members, as a list of NUL-terminated strings ending in an empty one.
   Strings rather than bytes because three of the classes have two-character
   members (ch, sh, th) and the suffix class has whole suffixes, and because a
   redefined class may contain characters outside ASCII -- these are compared
   as UTF-8, so a member is however many bytes its characters need.

   Table 22's defaults are English, which is the one thing about the rule
   language that does not travel: the vowels of a Spanish dictionary include
   the accented ones, and a rule engine that does not know that reads "mas"
   correctly and spells "mas" out letter by letter.  So they are defaults and
   not constants -- see rc_class_set and the #!class directive. */
static const char def_vowel[] = "a\0e\0i\0o\0u\0y\0";
static const char def_front[] = "e\0i\0y\0";
static const char def_cons[] =
  "b\0c\0d\0f\0g\0h\0j\0k\0l\0m\0n\0p\0q\0r\0s\0t\0v\0w\0x\0z\0";
static const char def_voiced[] = "b\0d\0g\0j\0l\0m\0n\0r\0v\0w\0z\0";
static const char def_at[] = "d\0j\0l\0n\0r\0s\0t\0z\0ch\0sh\0th\0";
static const char def_bang[] = "b\0c\0d\0f\0g\0p\0t\0";
static const char def_amp[] = "c\0g\0j\0s\0x\0z\0ch\0sh\0";
static const char def_digit[] = "0\0" "1\0" "2\0" "3\0" "4\0" "5\0" "6\0"
  "7\0" "8\0" "9\0";
/* Table 22's suffix list.  The parenthesised plurals are spelled out, and the
   "must also be followed by a non-alphabetic character" part is in the
   matcher rather than here. */
static const char def_suffix[] =
  "ables\0able\0ably\0edly\0eless\0elements\0element\0ements\0ement\0"
  "eness\0ely\0ers\0er\0ed\0es\0e\0ingly\0ings\0ing\0";

/* Which token each default belongs to; NULL for the structural ones, whose
   meaning is not a set of characters and so cannot be redefined. */
static const char *const rc_defaults[RC_NTOK] = {
  def_vowel, def_front, def_cons, NULL, NULL, def_voiced, def_at, def_bang,
  def_suffix, def_amp, NULL, NULL, def_digit, NULL, NULL
};

/* A class table.  Shared between the rules that were loaded under it, and
   copied on write, so that two dictionaries with different alphabets can be
   loaded into one rcdict and each keep its own -- which is the whole point of
   putting the classes in the file rather than in the profile. */
typedef struct rclasses
{
  int refs;			/* -1 on the shared default, never freed */
  char *set[RC_NTOK];		/* NULL: use rc_defaults[i] */
} rclasses;

static rclasses rc_default_classes = { -1, { NULL } };

static const char *
rc_class (const rclasses * c, int tok)
{
  if (c && c->set[tok])
    return c->set[tok];
  return rc_defaults[tok];
}

static void
rc_classes_unref (rclasses * c)
{
  int i;
  if (!c || c->refs < 0)
    return;
  if (--c->refs > 0)
    return;
  for (i = 0; i < RC_NTOK; i++)
    free (c->set[i]);
  free (c);
}

/* Copy before writing, so rules already loaded keep the table they were
   compiled against.  Returns NULL only on allocation failure. */
static rclasses *
rc_classes_own (rclasses * c)
{
  rclasses *n;
  int i;
  if (c && c->refs == 1)
    return c;			/* nobody else is looking at it */
  n = calloc (1, sizeof *n);
  if (!n)
    return NULL;
  n->refs = 1;
  for (i = 0; i < RC_NTOK; i++)
    if (c && c->set[i])
      {
	const char *p = c->set[i];
	size_t len = 0;
	while (p[len])
	  len += strlen (p + len) + 1;
	len++;			/* the terminating empty string */
	n->set[i] = malloc (len);
	if (!n->set[i])
	  {
	    rc_classes_unref (n);
	    return NULL;
	  }
	memcpy (n->set[i], c->set[i], len);
      }
  rc_classes_unref (c);
  return n;
}

/* --- UTF-8, only as much of it as the classes need ------------------------ */

/* A dictionary is UTF-8 and the text it rewrites is too, so "the character
   before the fragment" is not "the byte before the fragment".  Everything here
   walks characters; nothing decodes further than it has to. */
static size_t
uclen (const char *s, size_t avail)
{
  unsigned char c = (unsigned char) s[0];
  size_t n = 1;
  if (c >= 0xf0)
    n = 4;
  else if (c >= 0xe0)
    n = 3;
  else if (c >= 0xc0)
    n = 2;
  if (n > avail)
    n = 1;			/* malformed: treat the byte as itself */
  return n;
}

/* Start of the character ending at i, not going below lo. */
static size_t
uprev (const char *s, size_t lo, size_t i)
{
  size_t j = i;
  while (j > lo && ((unsigned char) s[j - 1] & 0xc0) == 0x80)
    j--;
  return j > lo ? j - 1 : lo;
}

/* Case-fold one character to a comparison key.  ASCII, and the Latin-1
   supplement, which is as far as a Table 5 target's own character set went and
   covers every accented letter a Spanish or French dictionary needs.  Anything
   beyond that compares unfolded rather than wrongly. */
static unsigned
fold_cp (const char *s, size_t n)
{
  unsigned cp;
  if (n == 1)
    cp = (unsigned char) s[0];
  else if (n == 2)
    cp = (unsigned) (((unsigned char) s[0] & 0x1f) << 6
		     | ((unsigned char) s[1] & 0x3f));
  else if (n == 3)
    cp = (unsigned) (((unsigned char) s[0] & 0x0f) << 12
		     | ((unsigned char) s[1] & 0x3f) << 6
		     | ((unsigned char) s[2] & 0x3f));
  else
    return 0xfffd;
  if (cp >= 'A' && cp <= 'Z')
    return cp + 32;
  /* C0-DE are the accented capitals; D7 is the multiplication sign, which is
     in the middle of them and is not a letter. */
  if (cp >= 0xc0 && cp <= 0xde && cp != 0xd7)
    return cp + 32;
  return cp;
}

/* Do the len bytes at a equal the NUL-terminated member b, case-folded? */
static int
same_chars (const char *a, size_t alen, const char *b)
{
  size_t i = 0, j = 0, blen = strlen (b);
  while (i < alen && j < blen)
    {
      size_t na = uclen (a + i, alen - i), nb = uclen (b + j, blen - j);
      if (fold_cp (a + i, na) != fold_cp (b + j, nb))
	return 0;
      i += na;
      j += nb;
    }
  return i == alen && j == blen;
}

/* Is the character (or characters) at s[0..avail) a member of the token's
   class?  Returns the number of BYTES matched, or 0 for no.  Longest member
   wins, which is what makes "ch" beat "c" in the @ and & classes. */
static size_t
rc_class_match (const rclasses * c, int tok, const char *s, size_t avail)
{
  const char *list = rc_class (c, tok);
  size_t best = 0;
  if (!list || !avail)
    return 0;
  for (; *list; list += strlen (list) + 1)
    {
      /* Compare member characters against the input, both folded. */
      size_t i = 0, j = 0, mlen = strlen (list);
      while (i < avail && j < mlen)
	{
	  size_t na = uclen (s + i, avail - i), nb = uclen (list + j,
							   mlen - j);
	  if (fold_cp (s + i, na) != fold_cp (list + j, nb))
	    break;
	  i += na;
	  j += nb;
	}
      if (j == mlen && i > best)
	best = i;
    }
  return best;
}

/* A letter is anything in the vowel class or the consonant class, and '$' is
   anything that is not.  Deriving it means a dictionary that adds the accented
   vowels to '#' gets '$' right for free -- and getting '$' wrong is not a
   small mistake: it is what makes a letter-naming rule such as $(m)$ fire in
   the middle of a word. */
static int
rc_is_letter (const rclasses * c, const char *s, size_t avail)
{
  return rc_class_match (c, RC_HASH, s, avail) != 0
    || rc_class_match (c, RC_HAT, s, avail) != 0;
}

/* One element of a context or fragment: a token, or a literal character. */
typedef struct
{
  unsigned char tok;		/* a Table 22 token, or 0 for a literal */
  unsigned char len;		/* literal length in bytes */
  char lit[5];			/* NUL-terminated: a UTF-8 character is <= 4 */
} ratom;

typedef struct rule
{
  match_kind kind;
  int nocase;			/* compare case-insensitively */
  int caps_only;		/* and require the input to be all capitals */
  char *pat;
  size_t patlen;
  /* M_RULE only: the compiled L(F)R, and the class table it was compiled
     against.  left is matched right-to-left from the fragment, right is
     matched left-to-right from the end of it. */
  ratom *left, *frag, *right;
  size_t nleft, nfrag, nright;
  rclasses *cls;
  int anypos;			/* fragment has no literal first byte */
  rcdict_regex *re;		/* M_REGEX only; compiled once, at load */
  size_t rseq;			/* ordinal among the regex rules */
  size_t seq;			/* load order, so first-match-wins can be
				   decided between the literal and regex
				   lists, which are searched separately */
  seg *segs;
  size_t nsegs;
  char *origin;			/* file name, for diagnostics */
  size_t line;
  struct rule *next;		/* next rule in the same first-byte bucket */
  struct rule *rnext;		/* next regex rule, in load order */
  struct rule *anext;		/* next any-position rule, in load order */
} rule;

struct rcdict
{
  const rcdict_profile *profile;
  rule **rules;			/* every rule, in load order, for freeing */
  size_t n, cap;
  /* Indexed by the lowercased first byte of the pattern.  Every rule that
     could match at a position is in bucket[tolower(input byte)], and within a
     bucket they stay in load order -- which is what keeps first-match-wins
     meaning what the file says it means.  (An Aho-Corasick automaton would be
     the textbook answer for many literals at once, but it reports matches in
     leftmost-longest order, and reconciling that with "the earlier rule wins"
     costs more than the scan it saves at these sizes.) */
  rule *head[256], *tail[256];
  /* Regex rules cannot be indexed by a first byte -- a pattern may begin with
     a class, an anchor or an alternation -- so they get their own list and are
     tried at every position.  Both lists are in load order, which is what lets
     match_at decide between them by sequence number. */
  rule *rhead, *rtail;
  size_t nregex;
  int has_regex;
  /* Rule matchers whose text fragment does not begin with a literal character
     -- an empty fragment, or one starting with the wildcard -- cannot be
     bucketed either, for the same reason.  The datasheet's ()= idiom, which
     ends a dictionary that is meant to replace the built in rules entirely,
     lands here. */
  rule *ahead, *atail;
  int has_anypos;
  int has_rules;		/* any M_RULE at all: see the lossy_punct note */
  /* The class table the next rules loaded will compile against.  Rules hold
     their own reference, so this only ever moves forward. */
  rclasses *classes;
};

rcdict *
rcdict_new (const rcdict_profile *p)
{
  rcdict *d = calloc (1, sizeof *d);
  if (!d)
    return NULL;
  d->profile = p ? p : &rcdict_rc8650;
  d->classes = &rc_default_classes;
  return d;
}

static void
free_rule (rule *r)
{
  size_t i;
  if (!r)
    return;
  for (i = 0; i < r->nsegs; i++)
    free (r->segs[i].s);
  free (r->segs);
  free (r->pat);
  free (r->origin);
  free (r->left);
  free (r->frag);
  free (r->right);
  rc_classes_unref (r->cls);
  rcdict_regex_free (r->re);
  free (r);
}

void
rcdict_clear (rcdict *d)
{
  size_t i;
  if (!d)
    return;
  for (i = 0; i < d->n; i++)
    free_rule (d->rules[i]);
  free (d->rules);
  d->rules = NULL;
  d->n = d->cap = 0;
  memset (d->head, 0, sizeof d->head);
  memset (d->tail, 0, sizeof d->tail);
  d->rhead = d->rtail = NULL;
  d->nregex = 0;
  d->has_regex = 0;
  d->ahead = d->atail = NULL;
  d->has_anypos = 0;
  d->has_rules = 0;
  rc_classes_unref (d->classes);
  d->classes = &rc_default_classes;
}

void
rcdict_free (rcdict *d)
{
  if (!d)
    return;
  rcdict_clear (d);
  free (d);
}

size_t
rcdict_rule_count (const rcdict *d)
{
  return d ? d->n : 0;
}

/* --- the format ----------------------------------------------------------- */

/* Diagnostics name the file and line, because a dictionary is edited by hand
   and "something is wrong somewhere" is not an actionable thing to be told. */
static void
report_line (rcdict_report r, void *ctx, size_t off, const char *name,
	     size_t line, const char *msg)
{
  char buf[256];
  size_t n;
  if (!r)
    return;
  n = (size_t) snprintf (buf, sizeof buf, "%s:%lu: %s",
			 name ? name : "<text>", (unsigned long) line, msg);
  if (n >= sizeof buf)
    buf[sizeof buf - 1] = 0;
  r (ctx, off, buf);
}

/* The pattern ends at an '=' with whitespace on both sides (or at end of
   line).  Anything else is part of the pattern, so "a = b" and "x=y" and ";"
   all mean what they look like.  A literal separator is written "\=". */
static const char *
find_separator (const char *s, size_t len)
{
  size_t i;
  for (i = 0; i < len; i++)
    {
      if (s[i] == '\\')
	{
	  i++;
	  continue;
	}
      if (s[i] == '=' && i > 0 && is_space ((unsigned char) s[i - 1])
	  && (i + 1 == len || is_space ((unsigned char) s[i + 1])))
	return s + i;
    }
  return NULL;
}

static void
trim (const char **s, size_t *len)
{
  while (*len && is_space ((unsigned char) **s))
    {
      (*s)++;
      (*len)--;
    }
  while (*len && is_space ((unsigned char) (*s)[*len - 1]))
    (*len)--;
}

/* Copy a pattern.  The ONLY escape it has is "\\=", and only in the untabbed
   spelling, where '=' separates the pattern from the output; tab-separated
   columns need no escaping at all.  Every other backslash survives untouched,
   which is not a nicety -- a regex is mostly backslashes, and collapsing them
   here would quietly turn \\b into b and \\. into any character. */
static char *
pattern_dup (const char *s, size_t len, int allow_eq_escape, size_t *outlen)
{
  char *o = malloc (len + 1);
  size_t i, j = 0;
  if (!o)
    return NULL;
  for (i = 0; i < len; i++)
    {
      if (allow_eq_escape && s[i] == '\\' && i + 1 < len && s[i + 1] == '=')
	o[j++] = s[++i];
      else
	o[j++] = s[i];
    }
  o[j] = 0;
  *outlen = j;
  return o;
}

static int
push_seg (rule *r, seg_kind kind, const char *s, size_t len, int n)
{
  seg *ns = realloc (r->segs, (r->nsegs + 1) * sizeof *ns);
  if (!ns)
    return 0;
  r->segs = ns;
  ns += r->nsegs;
  ns->kind = kind;
  ns->n = n;
  ns->len = len;
  ns->s = NULL;
  if (s)
    {
      ns->s = malloc (len + 1);
      if (!ns->s)
	return 0;
      memcpy (ns->s, s, len);
      ns->s[len] = 0;
    }
  r->nsegs++;
  return 1;
}

/* Parse the output template into segments.  Doing it once at load means a
   mistake is reported when the file is read rather than in the middle of
   speaking, and that expansion does no parsing at all. */
static int
parse_template (rule *r, const rcdict_profile *p, const char *s, size_t len,
		const char *name, size_t line, size_t off,
		rcdict_report report, void *ctx)
{
  size_t i = 0, lit = 0;
  char litbuf[RCDICT_MAX_LINE];

#define FLUSH_LIT()                                                     \
  do {                                                                  \
    if (lit && !push_seg (r, SEG_TEXT, litbuf, lit, 0)) return 0;        \
    lit = 0;                                                            \
  } while (0)

  while (i < len)
    {
      char c = s[i];

      if (c == '\\' && i + 1 < len)
	{
	  char e = s[i + 1];
	  if (e >= '0' && e <= '9')
	    {
	      FLUSH_LIT ();
	      if (!push_seg (r, SEG_BACKREF, NULL, 0, e - '0'))
		return 0;
	      if (e != '0' && r->kind != M_REGEX)
		report_line (report, ctx, off + i, name, line,
			     "\\1-\\9 need a matcher with capture groups; "
			     "only \\0 is available here");
	      i += 2;
	      continue;
	    }
	  if (lit + 1 < sizeof litbuf)
	    litbuf[lit++] = e;
	  i += 2;
	  continue;
	}

      if (c == '[' || c == '{')
	{
	  char close = (c == '[') ? ']' : '}';
	  size_t body = i + 1, e = body;
	  while (e < len && s[e] != close)
	    {
	      if (s[e] == '\\' && e + 1 < len)
		e++;
	      e++;
	    }
	  if (e >= len)
	    {
	      report_line (report, ctx, off + i, name, line,
			   c == '[' ? "unclosed [ in output"
			   : "unclosed { in output");
	      return 0;
	    }
	  FLUSH_LIT ();
	  if (c == '[')
	    {
	      if (!rcdict_check_phonemes (p, s + body, e - body, NULL, NULL))
		{
		  report_line (report, ctx, off + body, name, line,
			       "output contains a symbol that is not a "
			       "phoneme, modifier or pause");
		  return 0;
		}
	      if (!push_seg (r, SEG_PHONEME, s + body, e - body, 0))
		return 0;
	    }
	  else
	    {
	      if (e == body)
		{
		  report_line (report, ctx, off + i, name, line,
			       "empty {} in output");
		  return 0;
		}
	      if (!push_seg (r, SEG_COMMAND, s + body, e - body, 0))
		return 0;
	    }
	  i = e + 1;
	  continue;
	}

      if (lit + 1 < sizeof litbuf)
	litbuf[lit++] = c;
      i++;
    }
  FLUSH_LIT ();
#undef FLUSH_LIT
  return 1;
}

/* --- the rule matcher: L(F)R, RC8650 datasheet "Exception Syntax" --------- */

/* Compile one side of an exception into atoms.  In a context every Table 22
   token is a token; in the text fragment only the wildcard is, and the
   datasheet is explicit that any other token there is a literal. */
static int
compile_atoms (const char *s, size_t len, int in_fragment,
	       ratom ** out, size_t *nout)
{
  size_t i = 0, n = 0, cap = 0;
  ratom *a = NULL;

  while (i < len)
    {
      size_t cl = uclen (s + i, len - i);
      ratom one;
      memset (&one, 0, sizeof one);

      if (cl == 1 && (in_fragment ? s[i] == '`' : strchr (rc_tokens, s[i])
		      && s[i]))
	one.tok = (unsigned char) s[i];
      else
	{
	  if (cl > 4)
	    cl = 1;
	  one.len = (unsigned char) cl;
	  memcpy (one.lit, s + i, cl);
	}

      if (n == cap)
	{
	  ratom *na = realloc (a, (cap ? cap * 2 : 8) * sizeof *na);
	  if (!na)
	    {
	      free (a);
	      return 0;
	    }
	  a = na;
	  cap = cap ? cap * 2 : 8;
	}
      a[n++] = one;
      i += cl;
    }
  *out = a;
  *nout = n;
  return 1;
}

/* Split "L(F)R" and compile all three parts.  The fragment runs from the first
   '(' to the LAST ')', so a rule may pronounce a parenthesis: "(()" is a rule
   for '(' and "())" one for ')'. */
static int
compile_rule (rule *r, const char *pat, size_t len)
{
  const char *open = memchr (pat, '(', len), *close = NULL;
  size_t i;

  if (!open)
    return 0;
  for (i = len; i > (size_t) (open - pat); i--)
    if (pat[i - 1] == ')')
      {
	close = pat + i - 1;
	break;
      }
  if (!close)
    return 0;

  if (!compile_atoms (pat, (size_t) (open - pat), 0, &r->left, &r->nleft))
    return 0;
  if (!compile_atoms (open + 1, (size_t) (close - open - 1), 1,
		      &r->frag, &r->nfrag))
    return 0;
  if (!compile_atoms (close + 1, (size_t) (pat + len - close - 1), 0,
		      &r->right, &r->nright))
    return 0;

  /* Bucketable only if the fragment starts with a literal character. */
  r->anypos = !(r->nfrag && r->frag[0].len);
  return 1;
}

/* One character of input compared with one literal atom. */
static int
atom_is (const rule *r, const ratom * a, const char *s, size_t n)
{
  if (!r->nocase)
    return a->len == n && memcmp (a->lit, s, n) == 0;
  return same_chars (s, n, a->lit);
}

static int
is_nonprinting (unsigned char c)
{
  return c < 0x21;
}

/* Which Table 22 class does this token name?  -1 if it is not one. */
static int
tok_class (unsigned char tok)
{
  const char *q = tok ? strchr (rc_tokens, tok) : NULL;
  return q ? (int) (q - rc_tokens) : -1;
}

/* How many bytes of the class's longest member end at i, looking backwards?
   Classes may have two-character members (ch, sh, th), so this is not simply
   the length of the previous character. */
static size_t
class_match_back (const rclasses * c, int idx, const char *in, size_t lo,
		  size_t i)
{
  size_t take = 0, back;
  for (back = 1; back <= 8 && back <= i - lo; back++)
    if (rc_class_match (c, idx, in + i - back, back) == back)
      take = back;
  return take;
}

/* The context matchers recurse rather than loop, because the variable-width
   tokens have to be able to give a character back: "(a)*x" wants the run of
   consonants to stop one short of the x, and a greedy pass that never
   reconsidered would eat the x -- x being a consonant -- and then fail,
   silently, for as long as the dictionary lived.

   The budget is the same idea as the regex engine's: a pattern with several
   variable-width tokens in it can in principle be made to explode, and a
   dictionary must not be able to make a screen reader go quiet.  Contexts are
   a handful of atoms long in practice, so nothing real comes near it. */
#define RC_CTX_BUDGET 4000

static int
ctx_right (const rule *r, const char *in, size_t i, size_t end, size_t k,
	   int *budget)
{
  const rclasses *c = r->cls;
  const ratom *a;
  size_t avail, cl;

  if (k == r->nright)
    return 1;
  if (--*budget < 0)
    return 0;

  a = &r->right[k];
  avail = end - i;

  switch (a->tok)
    {
    case 0:			/* a literal character */
      if (!avail)
	return 0;
      cl = uclen (in + i, avail);
      if (!atom_is (r, a, in + i, cl))
	return 0;
      return ctx_right (r, in, i + cl, end, k + 1, budget);

    case '$':			/* a nonalphabetic character, or the end */
      if (!avail)
	return ctx_right (r, in, i, end, k + 1, budget);
      if (rc_is_letter (c, in + i, avail))
	return 0;
      return ctx_right (r, in, i + uclen (in + i, avail), end, k + 1, budget);

    case '`':			/* wildcard */
      if (!avail)
	return 0;
      return ctx_right (r, in, i + uclen (in + i, avail), end, k + 1, budget);

    case '*':			/* one or more consonants */
    case ':':			/* zero or more */
      {
	size_t stops[64], n = 0, j = i;
	stops[n++] = j;		/* having taken none */
	while (n < sizeof stops / sizeof *stops && j < end
	       && (cl = rc_class_match (c, RC_HAT, in + j, end - j)) != 0)
	  stops[n++] = j += cl;
	/* Longest first, which is what makes the common case one step. */
	while (n-- > (a->tok == '*' ? 1u : 0u))
	  if (ctx_right (r, in, stops[n], end, k + 1, budget))
	    return 1;
	return 0;
      }

    case '~':			/* one or more non-printing characters */
      {
	size_t j = i, n = 0;
	while (j < end && is_nonprinting ((unsigned char) in[j]))
	  {
	    j++;
	    n++;
	  }
	for (; n; n--)
	  if (ctx_right (r, in, i + n, end, k + 1, budget))
	    return 1;
	return 0;
      }

    case '|':			/* one or more digits; commas are ignored */
      {
	size_t stops[64], n = 0, j = i;
	while (n < sizeof stops / sizeof *stops && j < end)
	  {
	    if (in[j] == ',')
	      {
		j++;
		continue;
	      }
	    cl = rc_class_match (c, RC_BSLASH, in + j, end - j);
	    if (!cl)
	      break;
	    j += cl;
	    stops[n++] = j;	/* one stop per digit: '|' needs at least one */
	  }
	while (n--)
	  if (ctx_right (r, in, stops[n], end, k + 1, budget))
	    return 1;
	return 0;
      }

    case '%':			/* a suffix, and then a nonalphabetic */
      {
	const char *list = rc_class (c, RC_PCT);
	for (; list && *list; list += strlen (list) + 1)
	  {
	    size_t mlen = strlen (list);
	    if (mlen > avail || !same_chars (in + i, mlen, list))
	      continue;
	    if (i + mlen < end && rc_is_letter (c, in + i + mlen,
					        end - i - mlen))
	      continue;		/* Table 22: and then a non-letter */
	    if (ctx_right (r, in, i + mlen, end, k + 1, budget))
	      return 1;
	  }
	return 0;
      }

    default:			/* a character class */
      {
	int idx = tok_class (a->tok);
	if (idx < 0 || !avail)
	  return 0;
	cl = rc_class_match (c, idx, in + i, avail);
	if (!cl)
	  return 0;
	return ctx_right (r, in, i + cl, end, k + 1, budget);
      }
    }
}

/* The same atoms, matched right-to-left from the fragment.  Separate from
   ctx_right rather than one function with a direction flag: every step of it
   differs, and a UTF-8 character is walked backwards by a different means
   than forwards. */
static int
ctx_left (const rule *r, const char *in, size_t lo, size_t i, size_t k,
	  int *budget)
{
  const rclasses *c = r->cls;
  const ratom *a;
  size_t avail, ps;

  if (!k)
    return 1;
  if (--*budget < 0)
    return 0;

  a = &r->left[k - 1];
  avail = i - lo;

  switch (a->tok)
    {
    case 0:
      if (!avail)
	return 0;
      ps = uprev (in, lo, i);
      if (!atom_is (r, a, in + ps, i - ps))
	return 0;
      return ctx_left (r, in, lo, ps, k - 1, budget);

    case '$':
      if (!avail)		/* the start of the run is nonalphabetic */
	return ctx_left (r, in, lo, i, k - 1, budget);
      ps = uprev (in, lo, i);
      if (rc_is_letter (c, in + ps, i - ps))
	return 0;
      return ctx_left (r, in, lo, ps, k - 1, budget);

    case '`':
      if (!avail)
	return 0;
      return ctx_left (r, in, lo, uprev (in, lo, i), k - 1, budget);

    case '*':
    case ':':
      {
	size_t stops[64], n = 0, j = i, take;
	stops[n++] = j;
	while (n < sizeof stops / sizeof *stops && j > lo
	       && (take = class_match_back (c, RC_HAT, in, lo, j)) != 0)
	  stops[n++] = j -= take;
	while (n-- > (a->tok == '*' ? 1u : 0u))
	  if (ctx_left (r, in, lo, stops[n], k - 1, budget))
	    return 1;
	return 0;
      }

    case '~':
      {
	size_t j = i, n = 0;
	while (j > lo && is_nonprinting ((unsigned char) in[j - 1]))
	  {
	    j--;
	    n++;
	  }
	for (; n; n--)
	  if (ctx_left (r, in, lo, i - n, k - 1, budget))
	    return 1;
	return 0;
      }

    case '|':
      {
	size_t stops[64], n = 0, j = i, take;
	while (n < sizeof stops / sizeof *stops && j > lo)
	  {
	    if (in[j - 1] == ',')
	      {
		j--;
		continue;
	      }
	    take = class_match_back (c, RC_BSLASH, in, lo, j);
	    if (!take)
	      break;
	    j -= take;
	    stops[n++] = j;
	  }
	while (n--)
	  if (ctx_left (r, in, lo, stops[n], k - 1, budget))
	    return 1;
	return 0;
      }

    case '%':
      {
	/* The suffix ends where the fragment begins, so the "and then a
	   non-letter" half of Table 22's definition is about the fragment and
	   is left to the fragment. */
	const char *list = rc_class (c, RC_PCT);
	for (; list && *list; list += strlen (list) + 1)
	  {
	    size_t mlen = strlen (list);
	    if (mlen > avail || !same_chars (in + i - mlen, mlen, list))
	      continue;
	    if (ctx_left (r, in, lo, i - mlen, k - 1, budget))
	      return 1;
	  }
	return 0;
      }

    default:
      {
	int idx = tok_class (a->tok);
	size_t take;
	if (idx < 0 || !avail)
	  return 0;
	take = class_match_back (c, idx, in, lo, i);
	if (!take)
	  return 0;
	return ctx_left (r, in, lo, i - take, k - 1, budget);
      }
    }
}

/* The whole exception at in[i].  Sets *mlen to the fragment's length in bytes
   -- which is what the scanner consumes; the contexts are only looked at. */
static int
match_rule_at (const rule *r, const char *in, size_t lo, size_t i,
	       size_t end, size_t *mlen)
{
  size_t j = i, k;

  for (k = 0; k < r->nfrag; k++)
    {
      const ratom *a = &r->frag[k];
      size_t cl;
      if (j >= end)
	return 0;
      cl = uclen (in + j, end - j);
      if (a->tok == '`')	/* the wildcard is legal inside a fragment */
	j += cl;
      else if (atom_is (r, a, in + j, cl))
	j += cl;
      else
	return 0;
    }

  if (r->nleft)
    {
      int budget = RC_CTX_BUDGET;
      if (!ctx_left (r, in, lo, i, r->nleft, &budget))
	return 0;
    }
  if (r->nright)
    {
      int budget = RC_CTX_BUDGET;
      if (!ctx_right (r, in, j, end, 0, &budget))
	return 0;
    }

  *mlen = j - i;
  return 1;
}

static int
add_rule (rcdict *d, rule *r)
{
  unsigned char b;
  if (d->n == d->cap)
    {
      size_t nc = d->cap ? d->cap * 2 : 32;
      rule **nr = realloc (d->rules, nc * sizeof *nr);
      if (!nr)
	return 0;
      d->rules = nr;
      d->cap = nc;
    }
  r->seq = d->n;
  d->rules[d->n++] = r;
  if (r->kind == M_RULE)
    d->has_rules = 1;

  if (r->kind == M_REGEX)
    {
      if (d->rtail)
	d->rtail->rnext = r;
      else
	d->rhead = r;
      d->rtail = r;
      r->rseq = d->nregex++;
      d->has_regex = 1;
      return 1;
    }

  if (r->kind == M_RULE && r->anypos)
    {
      if (d->atail)
	d->atail->anext = r;
      else
	d->ahead = r;
      d->atail = r;
      d->has_anypos = 1;
      return 1;
    }

  /* An exception is bucketed by the first byte of its TEXT FRAGMENT, not of
     the pattern -- "$(e)r^" is reached from an e in the input, not from a
     dollar sign. */
  b = (unsigned char) (r->kind == M_RULE ? r->frag[0].lit[0] : r->pat[0]);
  if (b >= 'A' && b <= 'Z')
    b = (unsigned char) (b + 32);
  if (d->tail[b])
    d->tail[b]->next = r;
  else
    d->head[b] = r;
  d->tail[b] = r;
  return 1;
}

int
rcdict_add_text (rcdict *d, const char *src, size_t len, const char *name,
		 rcdict_report report, void *ctx)
{
  size_t i = 0, line = 0;
  int added = 0, default_nocase = 1;

  if (!d || !src)
    return 0;

  while (i < len)
    {
      size_t ls = i, le, off;
      const char *p, *raw;
      size_t plen, rawlen;
      const char *sep;

      while (i < len && src[i] != '\n')
	i++;
      le = i;
      if (le > ls && src[le - 1] == '\r')
	le--;			/* files written on Windows */
      if (i < len)
	i++;
      line++;

      off = ls;

      /* Two views of the line.  raw keeps its tabs, including a trailing one,
         because a trailing tab is an empty last column and trimming it away
         would silently turn a four-column line into a three-column one -- and
         so change which field is the pattern.  p is fully trimmed, and is what
         the emptiness, comment and directive tests use. */
      raw = src + ls;
      rawlen = le - ls;
      while (rawlen && raw[0] == ' ')
	{
	  raw++;
	  rawlen--;
	}
      while (rawlen && raw[rawlen - 1] == ' ')
	rawlen--;

      p = raw;
      plen = rawlen;
      trim (&p, &plen);

      if (!plen || *p == ';')
	continue;

      if (plen > RCDICT_MAX_LINE)
	{
	  report_line (report, ctx, off, name, line, "line too long, skipped");
	  continue;
	}

      /* Directives. */
      if (plen > 2 && p[0] == '#' && p[1] == '!')
	{
	  const char *a = p + 2;
	  size_t alen = plen - 2;
	  trim (&a, &alen);
	  if (alen >= 4 && !memcmp (a, "case", 4))
	    {
	      const char *v = a + 4;
	      size_t vlen = alen - 4;
	      trim (&v, &vlen);
	      if (vlen && (v[0] == 's' || v[0] == 'S'))
		default_nocase = 0;
	      else if (vlen && (v[0] == 'i' || v[0] == 'I'))
		default_nocase = 1;
	      else
		report_line (report, ctx, off, name, line,
			     "#!case wants 'sensitive' or 'insensitive'");
	    }
	  else if (alen >= 7 && !memcmp (a, "rcdict", 6))
	    {
	      const char *v = a + 6;
	      size_t vlen = alen - 6;
	      trim (&v, &vlen);
	      if (vlen != 1 || v[0] != '1')
		report_line (report, ctx, off, name, line,
			     "unknown #!rcdict version; reading it as 1");
	    }
	  else if (alen >= 4 && !memcmp (a, "lang", 4))
	    {
	      /* Recorded by convention, not yet acted on. */
	    }
	  else if (alen > 5 && !memcmp (a, "class", 5)
		   && is_space ((unsigned char) a[5]))
	    {
	      /* #!class <token> <member> <member> ...
	       *
	       * Redefines one Table 22 character class for every rule loaded
	       * after it.  Members are separated by spaces so that a class can
	       * hold the two-character ones the datasheet gives @ and & -- and
	       * so that a suffix class is writable at all.
	       *
	       * This is what makes the rule language usable outside English.
	       * Table 22's own vowel list is "aeiouy", and a Spanish
	       * dictionary that inherits it does not merely misplace a stress:
	       * an accented letter falls out of the alphabet altogether, so $
	       * matches inside a word and the rules meant for single letters
	       * start firing.  "mas" comes out spelled aloud. */
	      const char *v = a + 5;
	      size_t vlen = alen - 5;
	      const char *tokp;
	      trim (&v, &vlen);
	      if (!vlen || !(tokp = strchr (rc_tokens, v[0])) || !v[0])
		report_line (report, ctx, off, name, line,
			     "#!class wants a Table 22 token "
			     "(one of #+^*:?@!%&$~\\|`) and its members");
	      else if (!rc_defaults[tokp - rc_tokens])
		report_line (report, ctx, off, name, line,
			     "that class is structural and cannot be "
			     "redefined; $ follows # and ^, and * and : "
			     "follow ^");
	      else
		{
		  size_t idx = (size_t) (tokp - rc_tokens);
		  const char *m = v + 1;
		  size_t mlen = vlen - 1, w = 0;
		  char *set;
		  trim (&m, &mlen);
		  /* The member list, rewritten as NUL-separated strings with
		     an empty one on the end. */
		  set = malloc (mlen + 2);
		  if (!set)
		    return added;
		  {
		    size_t q = 0;
		    while (q < mlen)
		      {
			size_t s0;
			while (q < mlen && is_space ((unsigned char) m[q]))
			  q++;
			s0 = q;
			while (q < mlen && !is_space ((unsigned char) m[q]))
			  q++;
			if (q > s0)
			  {
			    memcpy (set + w, m + s0, q - s0);
			    w += q - s0;
			    set[w++] = 0;
			  }
		      }
		    set[w] = 0;
		  }
		  if (!w)
		    {
		      free (set);
		      report_line (report, ctx, off, name, line,
				   "#!class with no members; a class may not "
				   "be empty");
		    }
		  else
		    {
		      rclasses *nc = rc_classes_own (d->classes);
		      if (!nc)
			{
			  free (set);
			  return added;
			}
		      d->classes = nc;
		      free (nc->set[idx]);
		      nc->set[idx] = set;
		    }
		}
	    }
	  else
	    report_line (report, ctx, off, name, line, "unknown directive");
	  continue;
	}

      /* A rule, in either of two spellings.
       *
       * Tab-separated columns, which is the preferred one because a tab cannot
       * appear in any of the fields, so the pattern may contain spaces, '='
       * and anything else without escaping:
       *
       *     type <TAB> pattern <TAB> output
       *     type <TAB> flags <TAB> pattern <TAB> output
       *
       * Or, when the line has no tabs at all, the pattern and output are
       * separated by an '=' with whitespace either side and the flags ride on
       * the type after a colon:
       *
       *     type[:flags] pattern = output
       *
       * The second exists because tabs are invisible and editors, config
       * dialogs and web forms convert them to spaces without saying so.  A
       * dictionary that has been through one of those still loads.
       */
      {
	const char *t = p, *pat = NULL, *out = NULL, *flags = NULL;
	size_t tlen = 0, patlen = 0, outlen = 0, flagslen = 0;
	rule *r;
	int nocase = default_nocase, caps = 0, asked_nocase = 0;
	match_kind kind;
	int tabbed = memchr (raw, '\t', rawlen) != NULL;

	if (tabbed)
	  {
	    const char *f[5];
	    size_t fl[5];
	    int nf = 0;
	    const char *q = raw, *e = raw + rawlen;

	    while (nf < 5)
	      {
		const char *tab = memchr (q, '\t', (size_t) (e - q));
		f[nf] = q;
		fl[nf] = tab ? (size_t) (tab - q) : (size_t) (e - q);
		trim (&f[nf], &fl[nf]);
		nf++;
		if (!tab)
		  break;
		q = tab + 1;
	      }

	    if (nf > 4)
	      {
		report_line (report, ctx, off, name, line,
			     "too many tab-separated columns; want "
			     "type, [flags,] pattern, output");
		continue;
	      }
	    if (nf < 2)
	      {
		report_line (report, ctx, off, name, line,
			     "a rule needs at least a type and a pattern");
		continue;
	      }

	    t = f[0];
	    tlen = fl[0];
	    if (nf == 4)
	      {
		flags = f[1];
		flagslen = fl[1];
		pat = f[2];
		patlen = fl[2];
		out = f[3];
		outlen = fl[3];
		/* Catch a stray tab inside what was meant to be the pattern,
		   which would otherwise be read as a column of nonsense
		   flags. */
		{
		  size_t k;
		  for (k = 0; k < flagslen; k++)
		    if (flags[k] != 'i' && flags[k] != 'c' && flags[k] != 'C')
		      break;
		  if (k < flagslen)
		    {
		      report_line (report, ctx, off, name, line,
				   "four columns, but the second is not "
				   "flags (want i, c or C, or leave it "
				   "empty) -- is there a tab in the "
				   "pattern?");
		      continue;
		    }
		}
	      }
	    else			/* nf == 2 or 3 */
	      {
		pat = f[1];
		patlen = fl[1];
		if (nf == 3)
		  {
		    out = f[2];
		    outlen = fl[2];
		  }
		else
		  {
		    out = pat + patlen;	/* empty output: a silent match */
		    outlen = 0;
		  }
	      }
	  }
	else
	  while (tlen < plen && !is_space ((unsigned char) t[tlen])
		 && t[tlen] != ':')
	    tlen++;

	/* Flags ride on the type after a colon in the untabbed spelling, and may
	   do so in the tabbed one too. */
	{
	  const char *colon = memchr (t, ':', tlen);
	  if (colon)
	    {
	      if (!flags)
		{
		  flags = colon + 1;
		  flagslen = tlen - (size_t) (colon - t) - 1;
		}
	      tlen = (size_t) (colon - t);
	    }
	}

	if (tlen == 4 && !memcmp (t, "word", 4))
	  kind = M_WORD;
	else if (tlen == 4 && !memcmp (t, "text", 4))
	  kind = M_TEXT;
	else if (tlen == 5 && !memcmp (t, "regex", 5))
	  kind = M_REGEX;
	else if (tlen == 4 && !memcmp (t, "rule", 4))
	  kind = M_RULE;
	else
	  {
	    report_line (report, ctx, off, name, line,
			 "line does not start with a matcher "
			 "(word, text, regex or rule)");
	    continue;
	  }


	if (!tabbed)
	  {
	    const char *rest = t + tlen;
	    size_t restlen = plen - tlen;

	    if (restlen && *rest == ':')
	      {			/* type:flags -- the type scan stopped here */
		rest++;
		restlen--;
		flags = rest;
		flagslen = 0;
		while (flagslen < restlen
		       && !is_space ((unsigned char) rest[flagslen]))
		  flagslen++;
		rest += flagslen;
		restlen -= flagslen;
	      }

	    sep = find_separator (rest, restlen);
	    if (!sep)
	      {
		report_line (report, ctx, off, name, line,
			     "no ' = ' between pattern and output "
			     "(or separate the columns with tabs)");
		continue;
	      }
	    pat = rest;
	    patlen = (size_t) (sep - rest);
	    out = sep + 1;
	    outlen = (size_t) (rest + restlen - out);
	    trim (&pat, &patlen);
	    trim (&out, &outlen);
	  }

	{
	  size_t k;
	  for (k = 0; k < flagslen; k++)
	    switch (flags[k])
	      {
	      case 'i':
		nocase = 1;
		caps = 0;
		asked_nocase = 1;
		break;
	      case 'c':
		nocase = 0;
		caps = 0;
		break;
	      case 'C':
		caps = 1;
		nocase = 1;
		break;
	      default:
		report_line (report, ctx, off, name, line,
			     "unknown flag; want i, c or C");
	      }
	}

	if (!patlen)
	  {
	    report_line (report, ctx, off, name, line,
			 "empty pattern (columns are type, [flags,] "
			 "pattern, output)");
	    continue;
	  }

	r = calloc (1, sizeof *r);
	if (!r)
	  return added;
	r->kind = kind;
	r->nocase = nocase;
	r->caps_only = caps;
	r->line = line;
	if (name)
	  {
	    r->origin = malloc (strlen (name) + 1);
	    if (r->origin)
	      memcpy (r->origin, name, strlen (name) + 1);
	  }
	r->pat = pattern_dup (pat, patlen, !tabbed, &r->patlen);
	if (!r->pat || !r->patlen)
	  {
	    free_rule (r);
	    report_line (report, ctx, off, name, line,
			 "empty pattern (columns are type, [flags,] "
			 "pattern, output)");
	    continue;
	  }
	if (kind == M_RULE)
	  {
	    if (!compile_rule (r, r->pat, r->patlen))
	      {
		free_rule (r);
		report_line (report, ctx, off, name, line,
			     "a rule wants the datasheet's L(F)R form, with "
			     "the text fragment in parentheses -- e.g. "
			     "$(e)r^ or (ch)");
		continue;
	      }
	    r->cls = d->classes;
	    if (r->cls->refs >= 0)
	      r->cls->refs++;
	  }

	if (kind == M_REGEX)
	  {
	    /* Remimu has no case-insensitive mode, and lowercasing a pattern
	       is not safe -- it would turn \\W into \\w and \\B into \\b.  So
	       regex rules match case-sensitively whatever the file default
	       says, and only an explicit request gets a complaint. */
	    if (asked_nocase)
	      report_line (report, ctx, off, name, line,
			   "regex patterns always match case-sensitively; "
			   "write a class such as [Nn] instead of :i");
	    r->nocase = 0;

	    r->re = rcdict_regex_compile (r->pat);
	    if (!r->re)
	      {
		free_rule (r);
		report_line (report, ctx, off, name, line,
			     "regex does not compile (or is too long)");
		continue;
	      }
	  }

	if (!parse_template (r, d->profile, out, outlen, name, line,
			     (size_t) (out - src), report, ctx)
	    || !add_rule (d, r))
	  {
	    free_rule (r);
	    continue;
	  }
	added++;
      }
    }
  return added;
}

int
rcdict_add_file (rcdict *d, const char *path, rcdict_report report, void *ctx)
{
  FILE *f;
  char *buf;
  size_t cap = 0, len = 0, got;
  int added;

  if (!d || !path)
    return 0;
  f = fopen (path, "rb");
  if (!f)
    {
      if (report)
	{
	  char msg[300];
	  snprintf (msg, sizeof msg, "cannot open '%s'", path);
	  report (ctx, 0, msg);
	}
      return 0;
    }

  cap = 8192;
  buf = malloc (cap);
  if (!buf)
    {
      fclose (f);
      return 0;
    }
  while ((got = fread (buf + len, 1, cap - len, f)) > 0)
    {
      len += got;
      if (len == cap)
	{
	  char *nb = realloc (buf, cap * 2);
	  if (!nb)
	    break;
	  buf = nb;
	  cap *= 2;
	}
    }
  fclose (f);

  added = rcdict_add_text (d, buf, len, path, report, ctx);
  free (buf);
  return added;
}

int
rcdict_add_path_list (rcdict *d, const char *list, char sep,
		      rcdict_report report, void *ctx)
{
  int added = 0;
  const char *p = list;

  if (!d || !list)
    return 0;
  while (*p)
    {
      const char *e = strchr (p, sep);
      size_t n = e ? (size_t) (e - p) : strlen (p);
      if (n)
	{
	  char path[1024];
	  if (n < sizeof path)
	    {
	      memcpy (path, p, n);
	      path[n] = 0;
	      added += rcdict_add_file (d, path, report, ctx);
	    }
	}
      if (!e)
	break;
      p = e + 1;
    }
  return added;
}

/* --- the substitution rules ----------------------------------------------- */

/* A phoneme span breaks the letter-to-sound stage's context on both sides of
   itself.  That one fact is the whole of what follows.  Measured in
   dict-lab/RESULTS.md; with both rules applied the output is byte-identical to
   what text mode produces, at 1S, 5S and 9S alike.
 *
 * Right edge: if '.' or ',' follows the span, the chip no longer gives the
 * final word its sentence-final fall and rate halving (notes.md §12) -- 14709
 * samples become 12900.  Moving the punctuation inside the span as its Table 5
 * pause phoneme restores it exactly.
 *
 * Left edge: the article "a" loses its reduction, coming out as 0x0c (EY)
 * rather than 0x05 (AX).  Absorbing it into the span restores that too.  It is
 * the only word of fifty tested that does this -- "an", "the", "to", "of" and
 * forty-six others are untouched -- so this is a special case for one word and
 * not a heuristic.
 *
 * '!' '?' ';' and ':' have no Table 5 pause phoneme, so there is nothing to
 * move and the rising terminal (0x2f) becomes a falling one (0x5c).  The
 * substitution is still made -- a right word with a statement's intonation
 * beats a wrong word -- and the loss is reported so that a caller can see it.
 * Rebuilding the rise by hand is possible (Table 6's '/' emits the marker
 * where you write it) but needs stress placement that is not derivable from an
 * arbitrary phoneme string, so it is not done automatically.
 */

static int
absorbable_punct (const rcdict_profile *p, int c)
{
  return (c == '.' || c == ',') && strchr (p->pauses, c) != NULL;
}

static int
lossy_punct (int c)
{
  return c == '!' || c == '?' || c == ';' || c == ':';
}

/* Does in[ws..we) end with the article "a", standing as its own word? */
static int
ends_with_article (const char *in, size_t ws, size_t we, size_t *word_start)
{
  size_t e = we, s;
  while (e > ws && is_space ((unsigned char) in[e - 1]))
    e--;
  if (e == we)			/* no whitespace between the word and the span */
    return 0;
  s = e;
  while (s > ws && !is_space ((unsigned char) in[s - 1]))
    s--;
  if (e - s != 1 || upper ((unsigned char) in[s]) != 'A')
    return 0;
  *word_start = s;
  return 1;
}

static void
emit_span (sink * out, const rcdict_options *opt,
	   const char *inner, size_t innerlen,
	   int absorb_article, int pause_char, int continuing)
{
  const rcdict_profile *p = opt->profile;
  size_t s = 0, e = innerlen;

  while (s < e && is_space ((unsigned char) inner[s]))
    s++;
  while (e > s && is_space ((unsigned char) inner[e - 1]))
    e--;

  /* Continuing the span that is already open: take back the " <CTRL+A>T" that
     closed it instead of closing and reopening.  A letter-to-sound dictionary
     matches every character of every word, so without this the RCStudio
     Spanish sample produces 572 one-phoneme spans where it wants about ten,
     each paying for two mode switches. */
  if (continuing)
    unput (out, strlen (p->leave_phoneme) + 1);
  else
    puts_ (out, p->enter_phoneme);
  if (absorb_article)
    puts_ (out, " AX");
  if (e > s)
    {
      putc_ (out, ' ');
      put (out, inner + s, e - s);
    }
  if (pause_char)
    {
      putc_ (out, ' ');
      putc_ (out, (char) pause_char);
    }
  putc_ (out, ' ');
  puts_ (out, p->leave_phoneme);
}

/* --- matching ------------------------------------------------------------- */

static int
all_capitals (const char *s, size_t len)
{
  size_t i;
  int seen = 0;
  for (i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char) s[i];
      if (c >= 'a' && c <= 'z')
	return 0;
      if (c >= 'A' && c <= 'Z')
	seen = 1;
    }
  return seen;
}

static int
same (const char *a, const char *b, size_t n, int nocase)
{
  size_t i;
  if (!nocase)
    return memcmp (a, b, n) == 0;
  for (i = 0; i < n; i++)
    if (upper ((unsigned char) a[i]) != upper ((unsigned char) b[i]))
      return 0;
  return 1;
}

/* First rule, in load order, that matches at in[i].  run_start and run_end
   bound the text run: a match may not reach outside it, because what lies
   beyond is a command atom or an utterance boundary.

   That bound is also what makes the word-boundary test right.  A word rule
   asks whether the byte before the match is alphanumeric, and at the start of
   a run there is no such byte to ask about -- a command atom sitting there
   ends the word as surely as a space does, even though its last byte ("I" of
   an index marker) is a letter. */
static const rule *
match_at (const rcdict *d, const char *in, size_t run_start, size_t i,
	  size_t run_end, const char *runz, unsigned char *giveup,
	  size_t *mlen, rcap *caps, rcdict_report report, void *ctx)
{
  unsigned char b = (unsigned char) in[i];
  const rule *best = NULL, *r;
  size_t k;

  if (b >= 'A' && b <= 'Z')
    b = (unsigned char) (b + 32);

  for (r = d->head[b]; r; r = r->next)
    {
      if (r->kind == M_RULE)
	{
	  size_t n = 0;
	  if (!match_rule_at (r, in, run_start, i, run_end, &n) || !n)
	    continue;
	  if (r->caps_only && !all_capitals (in + i, n))
	    continue;
	  best = r;
	  *mlen = n;
	  break;
	}
      if (r->patlen > run_end - i)
	continue;
      if (!same (in + i, r->pat, r->patlen, r->nocase))
	continue;
      if (r->caps_only && !all_capitals (in + i, r->patlen))
	continue;
      if (r->kind == M_WORD)
	{
	  if (i > run_start && is_word_char ((unsigned char) in[i - 1]))
	    continue;
	  if (i + r->patlen < run_end
	      && is_word_char ((unsigned char) in[i + r->patlen]))
	    continue;
	}
      best = r;
      *mlen = r->patlen;
      break;			/* the bucket is in load order: first wins */
    }

  /* Regex rules live in their own list, so first-match-wins has to be settled
     between the two by load order.  A regex later in the file than a literal
     that already matched cannot win, so the search stops there. */
  if (d->has_regex && runz)
    for (r = d->rhead; r; r = r->rnext)
      {
	long n;
	size_t cs[RCDICT_NCAPS], cl[RCDICT_NCAPS];

	if (best && r->seq > best->seq)
	  break;
	if (giveup && (giveup[r->rseq >> 3] & (1u << (r->rseq & 7))))
	  continue;		/* this one already blew its budget */

	n = rcdict_regex_match (r->re, runz, i - run_start, RCDICT_NCAPS,
				cs, cl);

	/* Giving up is bounded per attempt, but the scanner tries every
	   position, so a catastrophic pattern would still cost the limit
	   times the length of the run -- 1.7 seconds of dead air on an
	   utterance-sized one, measured.  Retire the rule for the rest of
	   this call instead: the damage is then one budget, once, and the
	   user is told which line to look at. */
	if (n == RCDICT_REGEX_GAVEUP)
	  {
	    if (giveup)
	      giveup[r->rseq >> 3] |= (unsigned char) (1u << (r->rseq & 7));
	    if (report)
	      {
		char msg[256];
		snprintf (msg, sizeof msg,
			  "%s:%lu: regex gave up (runaway backtracking); "
			  "the rule is ignored for the rest of this text",
			  r->origin ? r->origin : "<text>",
			  (unsigned long) r->line);
		report (ctx, i, msg);
	      }
	    continue;
	  }

	/* A zero-length match would leave the scanner where it was, so it is
	   refused rather than allowed to spin. */
	if (n <= 0)
	  continue;
	if ((size_t) n > run_end - i)
	  continue;		/* cannot happen: runz ends at run_end */
	if (r->caps_only && !all_capitals (in + i, (size_t) n))
	  continue;

	best = r;
	*mlen = (size_t) n;
	if (caps)
	  for (k = 1; k < RCDICT_NCAPS; k++)
	    if (cs[k] != RCDICT_REGEX_UNSET)
	      {
		caps[k].start = run_start + cs[k];
		caps[k].len = cl[k];
	      }
	break;
      }

  /* Exceptions whose fragment has no literal first character are not in any
     bucket and are tried everywhere, settled against the others by load order
     exactly as the regex rules are. */
  if (d->has_anypos)
    for (r = d->ahead; r; r = r->anext)
      {
	size_t n = 0;
	if (best && r->seq > best->seq)
	  break;
	if (!match_rule_at (r, in, run_start, i, run_end, &n))
	  continue;
	/* A null text fragment matches without consuming anything, and the
	   scanner cannot stand still.  It takes one character instead, which
	   is the only reading under which the datasheet's own "()=" -- placed
	   last so that unmatched characters are ignored rather than falling
	   through to the built in rules -- does what it says. */
	if (!n)
	  n = uclen (in + i, run_end - i);
	if (r->caps_only && !all_capitals (in + i, n))
	  continue;
	best = r;
	*mlen = n;
	break;
      }

  if (best && caps)
    {
      caps[0].start = i;	/* \\0 is always the whole match */
      caps[0].len = *mlen;
    }
  return best;
}

/* --- emitting a rule's output --------------------------------------------- */

/* The absorption rules are about the phoneme span's edges, and a template can
   put text either side of one, so they apply to the FIRST segment (does it
   start with phonemes?) and the LAST (does it end with them?).  A template
   that is a single phoneme span -- which is what the inline escape is -- gets
   both, which is how the two paths stay consistent. */
static void
emit_rule (sink * out, const rcdict_options *opt, const rule *r,
	   const char *in, size_t run_start, size_t mstart, size_t mend,
	   size_t run_end, const rcap *caps, size_t *consumed_to,
	   int *span_open, int may_continue,
	   rcdict_report report, void *ctx)
{
  const rcdict_profile *p = opt->profile;
  size_t k;
  int absorb = 0, pause = 0;
  size_t article_at = 0;
  /* Does this output pick up a span the previous match left open? */
  int cont = may_continue && span_open && *span_open
    && r->nsegs && r->segs[0].kind == SEG_PHONEME;

  *consumed_to = mend;

  /* Absorbing the article ahead of the span is about the span's LEFT EDGE
     breaking the letter-to-sound context, and there is no such edge when the
     span was already open. */
  if (!cont && r->nsegs && r->segs[0].kind == SEG_PHONEME
      && ends_with_article (in, run_start, mstart, &article_at))
    {
      absorb = 1;
      unput (out, mstart - article_at);
    }

  if (r->nsegs && r->segs[r->nsegs - 1].kind == SEG_PHONEME)
    {
      size_t k2 = mend;
      while (k2 < run_end && is_space ((unsigned char) in[k2]))
	k2++;
      if (k2 < run_end && absorbable_punct (p, (unsigned char) in[k2]))
	{
	  pause = (unsigned char) in[k2];
	  *consumed_to = k2 + 1;
	}
      else if (k2 < run_end && lossy_punct ((unsigned char) in[k2])
	       && !(opt->dict && opt->dict->has_rules))
	/* Not warned about when the dictionary has exception rules in it: one
	   of those covers every character by construction, punctuation
	   included, so the '?' about to be complained of is on its way to a
	   pause phoneme of the dictionary's own choosing. */
	report_at (report, ctx, k2,
		   "span ends before ! ? ; or : which has no Table 5 pause "
		   "phoneme: the rising terminal is lost");
    }

  for (k = 0; k < r->nsegs; k++)
    {
      const seg *sg = &r->segs[k];
      switch (sg->kind)
	{
	case SEG_TEXT:
	  put (out, sg->s, sg->len);
	  break;
	case SEG_PHONEME:
	  emit_span (out, opt, sg->s, sg->len,
		     absorb && k == 0, k == r->nsegs - 1 ? pause : 0,
		     cont && k == 0);
	  break;
	case SEG_COMMAND:
	  putc_ (out, (char) p->cmd_char);
	  put (out, sg->s, sg->len);
	  break;
	case SEG_BACKREF:
	  /* \0 is the whole match, for any matcher.  \1-\9 are the groups a
	     regex captured; after any other matcher there are none, and they
	     expand to nothing rather than to something invented (which the
	     loader has already said out loud). */
	  if (caps && sg->n < (int) RCDICT_NCAPS && caps[sg->n].len)
	    put (out, in + caps[sg->n].start, caps[sg->n].len);
	  else if (sg->n == 0)
	    put (out, in + mstart, mend - mstart);
	  break;
	}
    }

  /* A rule with no output at all -- the silent (h)= of a Spanish dictionary --
     emits nothing, so whatever the span was doing it is still doing.

     An absorbed pause does not close the span either.  What Experiment 1
     measured is that the punctuation must be INSIDE the span (C3/C4), and it
     still is; and a dictionary whose own rules cover punctuation reaches the
     same place by the other path, "(,)=," matching and coalescing.  The two
     paths agreeing matters more than the caution would have bought. */
  if (span_open && r->nsegs)
    *span_open = r->segs[r->nsegs - 1].kind == SEG_PHONEME;
}

/* --- the scanner ---------------------------------------------------------- */

/* Length of the command atom starting at in[i], or 0 if this is not one.
   *newcmd is set when the atom changes the command character, *zap when it is
   the Zap command, which stops the chip honouring commands at all -- after
   which emitting a mode switch would have the chip speak "D" aloud. */
static size_t
command_atom (const char *in, size_t i, size_t len, unsigned char cmd,
	      int *newcmd, int *zap)
{
  size_t j;

  if (i >= len || (unsigned char) in[i] != cmd)
    return 0;
  if (i + 1 >= len)
    return 1;			/* trailing lead-in byte; pass it through */

  {
    unsigned char c = (unsigned char) in[i + 1];

    /* Doubled: the command character itself, to be spoken. */
    if (c == cmd)
      return 2;

    /* A different control character re-arms the lead-in as that character. */
    if (c >= 0x01 && c <= 0x1a)
      {
	*newcmd = c;
	return 2;
      }
  }

  /* <cmd><digits><letter>. */
  j = i + 1;
  while (j < len && is_digit ((unsigned char) in[j]))
    j++;
  if (j < len && is_alpha ((unsigned char) in[j]))
    {
      if (upper ((unsigned char) in[j]) == 'Z')
	*zap = 1;
      return j - i + 1;
    }
  if (j < len && ((unsigned char) in[j] == '?' || (unsigned char) in[j] == '*'
		  || (unsigned char) in[j] == '@'))
    return j - i + 1;		/* the query, DTMF and reinitialise commands */

  return 1;			/* malformed; hand the byte over untouched */
}

/* One run of ordinary text, bounded by whatever a rewrite must not cross.
   Both the inline escape and the dictionary are applied here, and both go
   through emit_rule, so the absorption rules cannot drift apart between
   them. */
static void
expand_run (sink * out, const rcdict_options *opt, const char *in,
	    size_t start, size_t end, unsigned char *giveup,
	    rcdict_report report, void *ctx)
{
  const rcdict_profile *p = opt->profile;
  size_t i = start;
  size_t openlen = opt->open ? strlen (opt->open) : 0;
  size_t closelen = opt->close ? strlen (opt->close) : 0;

  /* The regex engine takes a NUL-terminated string and no length, so a run
     has to be handed over as its own copy.  That is the right shape anyway:
     `^`, `$` and `\b` should see the edges of the speakable run and not the
     command bytes on either side of it, and without the copy a pattern could
     match straight through an index marker.

     Built once per run, and only when the dictionary actually has a regex in
     it, so the common case allocates nothing. */
  char stackbuf[1024];
  char *runz = NULL;
  const rcdict *dict = opt->dict;
  /* Whether the output currently ends in an open phoneme span, and the input
     offset at which it was left open.  Consecutive matches that both produce
     phonemes with nothing between them are joined into one span. */
  int span_open = 0;
  size_t span_at = start;

  if (dict && dict->has_regex && end > start)
    {
      size_t n = end - start;
      runz = (n < sizeof stackbuf) ? stackbuf : malloc (n + 1);
      if (runz)
	{
	  memcpy (runz, in + start, n);
	  runz[n] = 0;
	}
    }

  while (i < end)
    {
      if (opt->inline_phonemes && openlen && closelen
	  && i + openlen <= end && memcmp (in + i, opt->open, openlen) == 0)
	{
	  size_t body = i + openlen, endq = body, consumed;
	  seg one;
	  rule tmp;

	  while (endq < end
		 && !(endq + closelen <= end
		      && memcmp (in + endq, opt->close, closelen) == 0))
	    endq++;

	  if (endq + closelen > end)
	    {
	      /* Unterminated: not a span at all.  Pass the delimiter through
	         as text rather than swallowing the rest of the utterance. */
	      report_at (report, ctx, i, "unterminated phoneme escape");
	      put (out, in + i, openlen);
	      i += openlen;
	      continue;
	    }

	  /* Fail loudly on a bad symbol: speak the contents as ordinary text,
	     which is audible and obviously wrong, rather than sending the chip
	     into phoneme mode with something it will not say. */
	  if (!rcdict_check_phonemes (p, in + body, endq - body, report, ctx))
	    {
	      put (out, in + body, endq - body);
	      i = endq + closelen;
	      continue;
	    }

	  /* A one-segment rule, so this takes exactly the same path a
	     dictionary entry would.  emit_rule only reads the segment. */
	  memset (&tmp, 0, sizeof tmp);
	  memset (&one, 0, sizeof one);
	  one.kind = SEG_PHONEME;
	  one.s = (char *) (size_t) (in + body);
	  one.len = endq - body;
	  tmp.segs = &one;
	  tmp.nsegs = 1;

	  emit_rule (out, opt, &tmp, in, start, i, endq + closelen, end,
		     NULL, &consumed, &span_open, span_open && span_at == i,
		     report, ctx);
	  span_at = consumed;
	  i = consumed;
	  continue;
	}

      if (dict && dict->n)
	{
	  rcap caps[RCDICT_NCAPS];
	  size_t mlen = 0, k;
	  const rule *r;

	  for (k = 0; k < RCDICT_NCAPS; k++)
	    {
	      caps[k].start = 0;
	      caps[k].len = 0;
	    }
	  r = match_at (dict, in, start, i, end, runz, giveup, &mlen, caps,
			report, ctx);
	  if (r && mlen)
	    {
	      size_t consumed;
	      emit_rule (out, opt, r, in, start, i, i + mlen, end, caps,
			 &consumed, &span_open, span_open && span_at == i,
			 report, ctx);
	      span_at = consumed;
	      i = consumed;
	      continue;
	    }
	}

      /* Anything passed through as text closes the span. */
      span_open = 0;
      putc_ (out, in[i]);
      i++;
    }

  if (runz && runz != stackbuf)
    free (runz);
}

size_t
rcdict_expand (const rcdict_options *opt, const char *in, size_t inlen,
	       char *out_buf, size_t outcap, rcdict_report report, void *ctx)
{
  sink out;
  size_t i = 0;
  unsigned char cmd;
  int zapped = 0;
  /* One bit per regex rule, marking the ones that have blown their
     backtracking budget during this call; see match_at. */
  unsigned char stack_gu[64], *giveup = NULL;

  if (!opt || !opt->profile || (!in && inlen))
    return 0;
  cmd = opt->profile->cmd_char;

  if (opt->dict && opt->dict->nregex)
    {
      size_t nb = (opt->dict->nregex + 7) / 8;
      giveup = (nb <= sizeof stack_gu) ? stack_gu : calloc (nb, 1);
      if (giveup == stack_gu)
	memset (stack_gu, 0, nb);
    }

  out.buf = (out_buf && outcap) ? out_buf : NULL;
  out.cap = out.buf ? outcap : 0;
  out.len = 0;
  out.need = 0;

  while (i < inlen)
    {
      unsigned char c = (unsigned char) in[i];
      int newcmd = 0;
      size_t atom, run_end;

      /* An opaque command atom: copied verbatim, never matched into, and never
         matched across.  A substitution that spanned one would not mispronounce
         a word, it would corrupt a command -- a mangled index marker changes
         the chip's mode or eats the next byte as a parameter. */
      atom = command_atom (in, i, inlen, cmd, &newcmd, &zapped);
      if (atom)
	{
	  put (&out, in + i, atom);
	  if (newcmd)
	    cmd = (unsigned char) newcmd;
	  i += atom;
	  continue;
	}

      /* CTRL+^ restores command recognition after Zap. */
      if (c == 0x1e)
	{
	  zapped = 0;
	  putc_ (&out, (char) c);
	  i++;
	  continue;
	}

      /* An utterance boundary -- CR or NUL, the two bytes that make the chip
         speak what it has.  Nothing is rewritten across one. */
      if (c == '\r' || c == 0)
	{
	  putc_ (&out, (char) c);
	  i++;
	  continue;
	}

      /* A run of ordinary text, ending at the next thing a rewrite must not
         cross. */
      run_end = i;
      while (run_end < inlen)
	{
	  unsigned char c2 = (unsigned char) in[run_end];
	  int d1 = 0, d2 = 0;
	  if (c2 == '\r' || c2 == 0 || c2 == 0x1e)
	    break;
	  if (command_atom (in, run_end, inlen, cmd, &d1, &d2))
	    break;
	  run_end++;
	}

      /* Under Zap the chip speaks commands instead of obeying them, so a mode
         switch would be read out.  Rewriting stops entirely rather than being
         filtered down to the entries that happen to be text-only: Zap is rare,
         and "some of your dictionary applies" is a worse thing to explain than
         "none of it does". */
      if (zapped)
	put (&out, in + i, run_end - i);
      else
	expand_run (&out, opt, in, i, run_end, giveup, report, ctx);
      i = run_end;
    }

  if (giveup && giveup != stack_gu)
    free (giveup);
  if (out.buf && out.cap)
    out.buf[out.len < out.cap ? out.len : out.cap - 1] = 0;
  return out.need;
}
