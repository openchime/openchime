/* Typed emoticons turned into emoji in the composer (MARKDOWN.md §1, the
 * compose.emoticons setting). One small table both composers share: an
 * emoticon that stands as a word of its own -- whitespace or the start before
 * it, whitespace or the end after it -- and lies outside inline code and a
 * fenced block. Nothing on render: what was sent is what is shown. */
#ifndef OC_EMOTICON_H
#define OC_EMOTICON_H

#include <stddef.h>

/* The emoticon that ends at byte `caret` of `text` (`len` bytes, UTF-8): its
 * emoji, with its first byte in *start; NULL when there is none, it is part of a
 * longer word, or it lies in code. For a composer converting as a space is
 * typed: `caret` is where the space goes. */
const char *oc_emoticon_before(const char *text, size_t len, size_t caret, size_t *start);

/* `in` (`len` bytes) with every emoticon converted, as on send, into `out` (NUL
 * terminated). Returns how many were converted, or -1 when `out` (of `cap`
 * bytes) cannot hold the result, in which case nothing is converted. */
int oc_emoticon_convert(const char *in, size_t len, char *out, size_t cap);

/* Whether a person's compose.emoticons value (NULL where it was never set) turns
 * the conversion on: off unless it says "on". */
int oc_emoticon_setting_on(const char *value);

/* What a composer calls, with that answer in `on`, so the setting decides in one
 * place. As a space is typed: oc_emoticon_before, or NULL when off. */
const char *oc_emoticon_typed(int on, const char *text, size_t len, size_t caret, size_t *start);

/* On send: oc_emoticon_convert, or when off `in` copied as it is, returning 0. */
int oc_emoticon_send(int on, const char *in, size_t len, char *out, size_t cap);

#endif
