# Writing pronunciation dictionaries

A pronunciation dictionary is a plain text file that rewrites words on their way
to the synthesizer. You can use it to fix a name it says wrongly, expand an
abbreviation, spell something out phonetically, or slow it down for an acronym
that goes past too fast.

Dictionary files are named with a `.dict` extension. Where you put them, and how
you tell the program about them, depends on which program you are using — see
its own documentation for that. Everything else about them is the same
everywhere, and is what this guide covers.

---

## A first dictionary

```
#!rcdict 1
#!case insensitive

; My dictionary. Lines starting with ';' are comments.
; The columns are separated by TAB characters.

word	Sean	Shon
word	Mbps	megabits per second
word	NVDA	[EH N V IY D IY EY]
```

Three rules. The first two are **respellings** — you write the word the way it
should sound, and it goes back through the synthesizer's own letter-to-sound
rules. The third is **phonemes**, for when respelling cannot get you there.

Respelling first, always. It is easier to write, easier to read six months
later, and it keeps all of the synthesizer's natural rhythm and intonation.
Reach for phonemes only when no spelling gives you the sound you want.

---

## The shape of a file

A dictionary is line-oriented UTF-8 text. There are three kinds of line.

### Comments

A line whose first non-blank character is `;` is a comment. Comments must be
lines of their own — you cannot put one at the end of a rule — which means a
`;` inside a rule needs no escaping.

```
; This is a comment.
```

### Directives

Lines beginning `#!` set something for the whole file. All of them are
optional, but the first two are worth writing down every time.

```
#!rcdict 1                  the format version
#!case insensitive          the file's default: ignore capitals (this is the default)
#!case sensitive            ...or match capitals exactly
#!lang en                   the language, recorded for humans; nothing acts on it yet
```

### Rules

Everything else is a rule, and a rule is a row of columns:

```
type <TAB> flags <TAB> pattern <TAB> output
```

**The columns are separated by tab characters, not spaces.** That is the one
thing to get right. Because a tab can never appear inside a column, your pattern
and your output can contain spaces, `=`, `;` and anything else without needing
to be escaped.

The **flags** column may be left empty, or left out altogether for a
three-column line:

```
word	 	US	[Y UW EH S]      four columns, flags column empty
word	Sean	Shon                             three columns, no flags column
word	(sic)                                    two columns: matches, says nothing
```

A trailing tab is a real empty last column and does change the meaning of the
line. No other whitespace matters.

Two things follow from that, and both catch people out:

- **One tab per column.** Two tabs in a row is an *empty column*, not spacing.
  Lining your rules up prettily with extra tabs gives you too many columns and
  the line is rejected. Use one tab, or use the empty flags column deliberately.
- **There are no end-of-line comments.** Anything after the output column's tab
  is still part of the output and will be spoken. Put remarks on their own `;`
  line above the rule.

### If your editor eats tabs

Tabs are invisible, and text boxes, web forms and some editors turn them into
spaces without telling you. So a line **with no tabs anywhere in it** is read a
second way, and means exactly the same thing:

```
type:flags pattern = output
```

for example:

```
word Sean = Shon
word:C US = [Y UW EH S]
```

Here the pattern ends at an `=` with a space on either side, and `\=` is a
literal equals sign. Both spellings work, and you can mix them in one file. Use
tabs when you can.

---

## Matching: what to look for

The first column says what kind of pattern it is.

### `word` — whole words only

```
word	cat	kat
```

Matches `cat`, and `cat.` and `(cat)`, but not `cats`, `bobcat` or `cat5`. A
word is bounded by anything that is not a letter or a digit.

This is what you want almost every time.

### `text` — any run of characters

```
text	Mbps	megabits per second
```

Matches anywhere, including in the middle of a word. Useful for units and
suffixes, and dangerous for anything short: a `text` rule for `it` will also
fire inside `bitmap` and `politics`.

### `regex` — a regular expression

```
regex	\bv([0-9]+)\.([0-9]+)\b	version \1 point \2
regex	([0-9]+)%	\1 percent
```

For patterns you cannot express as a fixed string. The pieces you captured in
round brackets come back in the output as `\1` to `\9`.

Supported: capture groups, alternation `|`, character classes `[a-z]`, anchors
`^` and `$`, word boundaries `\b` and `\B`, the quantifiers `*` `+` `?` and
`{n,m}`, and lazy (`*?`) and possessive (`*+`) forms.

Two things to know about regular expressions here:

- **They always match capitals exactly**, whatever `#!case` says, and whatever
  flags you write. There is no case-insensitive mode. Write a character class
  instead: `[Vv]ersion`.
- A pattern that takes too long to try is abandoned, reported, and switched off
  for the rest of that piece of text, so a runaway expression cannot make the
  synthesizer stall. If a regex rule seems to work sometimes and not others,
  this is the first thing to suspect — simplify it.

### `rule`

Recognised but **not implemented yet**. A `rule` line loads without stopping the
rest of the file, and is reported and skipped.

---

## Flags: capitals

The second column takes any of three letters.

| Flag | Meaning |
|---|---|
| `i` | Ignore capitals. The default, unless the file says `#!case sensitive`. |
| `c` | Match capitals exactly. |
| `C` | Ignore the capitals in your *pattern*, but match only where the *text* is in all capitals. |

`c` and `C` both keep an acronym from swallowing an ordinary word:

```
word	C	US	[Y UW EH S]
word	C	IT	[AY T IY]
```

`US` and `IT` are spelled out; `us`, `it` and `Us` at the start of a sentence
are left alone. Written with `c` and an all-capitals pattern these two rules
would behave the same way. The difference is only in what you have to type
correctly: with `C` the pattern's own capitals do not matter, so `word C us` and
`word C Us` are the same rule.

---

## The output: what to say instead

The last column is a template. Most of it is ordinary text, and ordinary text is
spoken through the synthesizer's own letter-to-sound rules — which is why a
respelling is just a word.

Inside it, four things are special.

### `[ ... ]` — phonemes

```
word	arthritis	[AA R TH R AY DX IX S]
```

Everything between the brackets is a phoneme string (see the table below). The
synthesizer is switched into phoneme mode for it and back out afterwards; you do
not write the switches yourself.

Every symbol is checked when the file is loaded. A typo is reported with its
line number and the rule is skipped — it can never reach the synthesizer as
something unintelligible.

### `{ ... }` — a command

```
word	ASAP	{2S}[EY EH S EY P IY]{5S}
```

Whatever is inside the braces is sent to the synthesizer as one of its own
commands. The example drops to speed 2 for the acronym, so a string of letters
that flies past at normal speed is read deliberately.

Note what the second command is **not** doing. `{5S}` sets the speed to 5; it
does not restore whatever the speed was before. A command changes the
synthesizer for everything that follows, so a rule that changes a setting has to
name the value it wants to come back to — and only makes sense where you know
what that value is.

Commands are passed through as written and are **not** checked, so this is the
one part of the format where a mistake can produce something odd. Look up the
command letters in your synthesizer's documentation.

### `\0` to `\9` — what was matched

`\0` is the whole of the matched text. `\1` to `\9` are the round-bracket groups
of a `regex` rule, numbered left to right, and are only available there.

```
regex	([0-9]+)%	\1 percent
word	kg	\0 ilograms
```

### `\` — literals

`\[ \] \{ \} \\` give you those characters as themselves, and `\=` a literal
equals sign in the untabbed spelling. Anything else after a backslash is simply
the character that follows it.

### Mixing them

An entry has one output field, not two, so text and phonemes go in together:

```
word	Dr	[D AA K T ER]
text	approx	approximately
```

### An empty output

Leave the output off entirely and the match is **silent** — the matched text is
removed and nothing is said in its place:

```
text	(sic)
```

---

## Phonemes

Phoneme symbols are written in capitals, separated by spaces, between `[` and
`]`.

### Vowels

| Symbol | As in | | Symbol | As in |
|---|---|---|---|---|
| `AA` | f**a**ther, h**o**t | | `IY` | b**ee**t, s**ee** |
| `AE` | c**a**t, b**a**d | | `OW` | b**oa**t, g**o** |
| `AH` | b**u**t, c**u**p | | `OY` | b**oy**, c**oi**n |
| `AW` | **ou**t, h**ow** | | `UH` | b**oo**k, p**u**t |
| `AX` | **a**bout (unstressed) | | `UW` | b**oo**t, f**oo**d |
| `AY` | b**i**te, m**y** | | `EW` | same sound as `UW` |
| `EH` | b**e**t, s**ai**d | | `EI` | same sound as `EY` |
| `ER` | b**ir**d, h**er** | | `IH` | b**i**t, s**i**t |
| `EY` | b**a**ke, d**ay** | | `IX` | ros**e**s (unstressed) |

### Consonants

| Symbol | As in | | Symbol | As in |
|---|---|---|---|---|
| `B` `D` `F` `G` | as spelled | | `NG` | si**ng** |
| `H` `K` `L` `M` | as spelled | | `NY` | o**ni**on |
| `N` `P` `R` `S` | as spelled | | `SH` | **sh**oe |
| `T` `V` `W` `Z` | as spelled | | `ZH` | mea**s**ure |
| `CH` | **ch**urch | | `TH` | **th**in |
| `J` | **j**udge | | `DH` | **th**is, **th**em |
| `YY` | **y**es | | `WH` | **wh**ich |
| `DX` | bu**tt**er (a flap) | | `RR` | a rolled `R` |
| `KX` `PX` `TX` | variants of `K`, `P` and `T` — the synthesizer normally picks these itself |

### Single letters

`A` `E` `I` `O` `U` and `Y` are **also** phoneme symbols, and they are *not* the
letters' names:

| Written | Is the same sound as |
|---|---|
| `A` | `AA` (f**a**ther) |
| `E` | `EH` (b**e**t) |
| `I` | `IY` (b**ee**t) |
| `O` | `OW` (b**oa**t) |
| `U` | `UW` (b**oo**t) |
| `Y` | `YY` (**y**es) |

So `[K A T]` says "cot", not "cat". You want `[K AE T]`. When in doubt, use the
two-letter symbols — they are unambiguous.

Several other symbols are also the same sound as each other, which means
choosing between them changes nothing: `AH` and `AX`; `I`, `IY`; `IH` and `IX`;
`EW`, `U` and `UW`; `O` and `OW`; `A` and `AA`; `E` and `EH`; `EI` and `EY`.

### Pauses

`.` and `,` may be written inside a phoneme string as pauses — `,` a short one,
`.` a longer one.

> On the RC8650 only, `'` is available as a very short pause. The DoubleTalk PC
> does not have it, and a dictionary using it will report an error there. Leave
> it out of any dictionary you intend to share.

### Stress and pitch

Six characters — `/` `\` `+` `-` `>` `<` — are accepted inside a phoneme string
as attribute modifiers, along with a number from 0 to 99 which sets the pitch at
that point.

These attach directly to what follows them rather than standing as symbols of
their own, and are written with no space, so `-/D>/EH R` is `-`, `/`, `D`, `>`,
`/`, `EH`, space, `R`.

`/` places a rising-pitch marker exactly where you write it, which is the one of
the six with a documented use here: it is how you would rebuild the rise on a
question by hand. For what the others do, see the attribute modifier table in
your synthesizer's own documentation — they change pitch and stress rather than
timing, and are fiddly enough that they are worth reading up on rather than
guessing at.

**Most dictionaries need none of this.** The synthesizer applies its own stress
and intonation to a phoneme string exactly as it does to ordinary text, so
leaving them out is normally the right answer and is what these examples do.

---

## Order decides everything

**Rules are tried from the top of the file downwards, and the first one that
matches wins.** Nothing looks for the longest or the best match — it takes the
first.

That makes ordering the single most important thing about a dictionary, and it
catches people out in one particular way: a short pattern placed above a longer
one that starts the same way means the longer one is never reached.

```
text	RAT	rodent
text	RATING	score
```

`RATING` never fires: scanning reaches the `R`, `RAT` matches there, and that is
the end of it — the result is "rodentING". Turn them round:

```
text	RATING	score
text	RAT	rodent
```

**Put your specific rules above your general ones.** If a rule is not firing,
this is almost always why.

The same applies when you use more than one dictionary file at once: they are
all read into a single list, in the order they are loaded, and the first match
in that combined list wins. A file loaded later can therefore only *add* rules —
it can never override one in a file loaded before it. To beat an existing rule,
your file has to be loaded first.

Two more things follow from this model:

- **Scanning goes left to right through the text**, and once a rule has fired,
  scanning carries on *after* the text it matched.
- **Output is never looked at again.** A rule cannot match something another
  rule produced, so rules cannot trigger each other, and there is no way to
  write a loop.

---

## Things worth knowing

**Your text is not the only thing in the stream.** By the time a dictionary
runs, the program may have put its own commands into the text. A match will
never run across one of these, and never inside one — so a rule cannot corrupt a
command, and in exchange, a rule cannot match a phrase that happens to have one
in the middle of it. In practice they fall between chunks of text and you will
not notice.

**A word cannot be substituted across a line ending.** Matching stops at the end
of each piece of text the program sends.

**Mind the `!` `?` `;` and `:`.** A phoneme string that ends immediately before
one of these loses the rising intonation that a question would normally get,
because there is no pause symbol to carry it. The substitution is still made —
the right word said flatly beats the wrong word — but if a phrase matters, a
respelling will keep the intonation where phonemes will not.

**One bad line costs one rule.** A line that does not parse, or a phoneme
symbol that does not exist, is reported with its file name and line number and
skipped; the rest of the file loads normally. Check your program's log if a rule
is not doing anything.

**Nothing here can silence the synthesizer.** If a dictionary cannot be read, or
a rule cannot be applied, the text is spoken unchanged.

---

## Cheat sheet

This is a working dictionary. Every line uses exactly one tab between columns,
and the remarks are on their own comment lines because the format has no
end-of-line comments.

```
#!rcdict 1
#!case insensitive

; columns are TAB separated:  type <TAB> flags <TAB> pattern <TAB> output
; (the flags column may be empty, or left out entirely)

; respell a name
word	Sean	Shon
; expand an abbreviation
word	Mbps	megabits per second
; phonemes
word	NVDA	[EH N V IY D IY EY]
; only when written in capitals
word	C	US	[Y UW EH S]
; slow down for an acronym, then put the speed back
word	ASAP	{2S}[EY EH S EY P IY]{5S}
; text matches inside words too
text	Kbps	kilobits per second
; no output at all: matched and said silently
text	(sic)
; keep what was captured
regex	([0-9]+)%	\1 percent

; no tabs available? this means the same as the Mbps line above:
word Mbps = megabits per second
```

| | |
|---|---|
| `word` | whole words only |
| `text` | any substring |
| `regex` | regular expression, `\1`–`\9` for the groups, always case-sensitive |
| `i` `c` `C` | ignore capitals / match them exactly / match only all-capitals text |
| `[ ]` | phonemes |
| `{ }` | a synthesizer command |
| `\0` | the text that matched |
| `\[ \] \{ \} \\` | those characters, literally |
| first match wins | so put specific rules above general ones |
