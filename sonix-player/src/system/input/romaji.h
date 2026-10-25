#ifndef ROMAJI_H
#define ROMAJI_H

#include <stdbool.h>
#include <stddef.h>

// Live romaji-to-kana conversion for the Japanese keyboard's QWERTY panel.
//
// The letters a keyboard types are ASCII, and what an IME does with them is
// watch the tail of the text: an unconverted run of romaji sits there plainly
// ("k"), and each new letter either completes it into kana ("ka" -> か) or
// keeps it waiting ("n" waits, because it may end a word or begin な行). This
// module is that watch, kept free of LVGL so it can be checked in a plain C
// test: the caller owns the field, and hands over only the ASCII letters at
// its end.
//
// `tail` is that run of lower-case letters -- up to ROMAJI_TAIL_MAX of them,
// the letter just typed included. `katakana` chooses the script the completed
// syllables come out in (the shift key's meaning on the Japanese keyboard).
//
// The return is how many bytes of `tail` were consumed, and `emit` receives
// the UTF-8 kana to put in their place -- empty when nothing was complete.
// The caller rewrites the tail: the consumed bytes come out, `emit` goes in,
// and whatever the conversion did not take -- the part still waiting -- goes
// back after it, still in ASCII, exactly as an IME leaves its composition.
//
// A zero return is common and means nothing: most letters are mid-syllable
// ("k", "sh"), and a tail no rule can finish ("tsky") simply keeps waiting
// until a later letter makes sense of it.
int romaji_tail(const char *tail, bool katakana, char *emit, size_t emit_size);

// The same conversion, spelled as the caller sees it arrive: `pending` is the
// run before the letter, `c` the letter itself. Convenience for tests and for
// callers that have not put the letter in the field yet.
int romaji_feed(const char *pending, char c, bool katakana, char *emit, size_t emit_size);

// The conversion an ending provokes: a lone trailing "n" is an unfinished ん,
// and a space (or any word boundary) turns it into one. Returns how many
// bytes of `pending` were consumed -- 1 for that "n", 0 when there is nothing
// to flush -- with the kana in `emit`.
int romaji_flush(const char *pending, bool katakana, char *emit, size_t emit_size);

// The longest tail the conversion can still make sense of: four letters,
// because a waiting digraph plus the rule-もじ before it is what "nkya" needs
// and nothing longer converts in one step.
#define ROMAJI_TAIL_MAX 4

#endif // ROMAJI_H
