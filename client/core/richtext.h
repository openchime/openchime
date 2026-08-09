/*
 * OpenChime client core — message formatting (REQ-220, ARCH-100).
 *
 * The parser for the dialect [MARKDOWN.md](../../docs/MARKDOWN.md) specifies:
 * a Slack-compatible subset of inline emphasis extended with real lists.
 *
 * Client-side and nowhere else. Formatting needs no server knowledge, which is
 * exactly where it differs from @mentions (ARCH-89) — those had to resolve in
 * `shared/` because only the daemon holds the roster. It lives in `client/core/`
 * rather than `shared/` because it is shared between *frontends*, not with the
 * daemon, and one implementation is what stops the TUI and the GUI drifting
 * about what counts as bold.
 *
 * It returns SPANS OVER THE ORIGINAL BYTES and never a rewritten string. The
 * body a client renders is byte-identical to the body the daemon stored, which
 * keeps FTS5 search, migration 0021's mention offsets and message editing all
 * addressing the same text.
 */

#ifndef OC_RICHTEXT_H
#define OC_RICHTEXT_H

#include <stddef.h>
#include <stdint.h>

/* Styles. A span carries exactly one construct; OC_RT_DELIM is OR-ed in to say
 * "these bytes are the construct's delimiter, not its content" so a frontend
 * can dim `*`…`*` while keeping a `- ` bullet visible. */
enum {
    OC_RT_BOLD      = 0x0001,   /* *bold* or **bold** */
    OC_RT_ITALIC    = 0x0002,   /* _italic_ */
    OC_RT_STRIKE    = 0x0004,   /* ~struck~ */
    OC_RT_CODE      = 0x0008,   /* `inline code` */
    OC_RT_CODEBLOCK = 0x0010,   /* ```fenced```, may span lines */
    OC_RT_QUOTE     = 0x0020,   /* > quoted */
    OC_RT_BULLET    = 0x0040,   /* - item */
    OC_RT_ORDERED   = 0x0080,   /* 1. item */
    OC_RT_LINK      = 0x0100,   /* a bare http(s) URL, autolinked */

    OC_RT_DELIM     = 0x8000    /* the delimiter bytes of the construct above */
};

typedef struct {
    size_t   start;   /* byte offset into the body */
    size_t   len;     /* bytes */
    uint16_t style;   /* one OC_RT_* construct, optionally | OC_RT_DELIM */
} oc_rt_span;

/* Per message. Generous for real use; a bound so a pathological body cannot
 * make a keystroke do unbounded work — the composer re-scans on every one. */
#define OC_RT_MAX 128

/* Scan `body` (`len` bytes, not necessarily NUL-terminated) and write up to
 * `max` spans to `out`. Returns how many were found, which MAY EXCEED `max` so
 * a caller can tell it was truncated. `out` may be NULL when `max` is 0.
 *
 * Spans NEST rather than partition: `*bold with _italic_ inside*` yields a bold
 * span and an italic span inside it, and the style at a byte is the OR of every
 * span containing it. They arrive sorted by `start` ascending, and where two
 * share a start the enclosing one comes first.
 *
 * The rules, stated once so both frontends inherit them (MARKDOWN.md §2):
 *   - An INLINE opening delimiter must be preceded by whitespace, a line start,
 *     an opening bracket or another delimiter, and followed by a non-space; a
 *     closing one must be preceded by a non-space. So `2 * 3 * 4` is
 *     arithmetic and `a_variable_name` is an identifier.
 *   - Inline emphasis must close ON THE SAME LINE. A fenced code block need
 *     not; it is the only multi-line construct.
 *   - `\*` `\_` `\~` `` \` `` `\>` `\-` `\\` produce the literal character. The
 *     backslash is reported as a lone OC_RT_DELIM span so a frontend can hide
 *     it; the character after it is plain text.
 *   - Code suppresses everything: no markup, and no escapes, inside a code span
 *     or a fenced block.
 *   - Block markers (`>`, `- `, `1. `) are matched at the start of a line,
 *     after optional indentation, and are exempt from the inline rules.
 *   - A bare `http://` or `https://` URL is autolinked: it yields one
 *     OC_RT_LINK span over the address, with NO delimiter span, because there
 *     is no markup to hide — the address is its own label (MARKDOWN.md §4).
 *     Trailing sentence punctuation and an unbalanced closing bracket are left
 *     outside the span, so `(see https://example.com/a).` links the address and
 *     not the full stop. Only those two schemes are recognised: a link span is
 *     what a frontend turns into something the OS opens, so widening the set is
 *     a security decision and it is taken HERE rather than per frontend.
 *

 * Anything unmatched or ambiguous degrades to its literal source: an unclosed
 * delimiter yields no span at all, so a half-typed `*` never restyles the rest
 * of the message.
 */
size_t oc_rt_scan(const char *body, size_t len, oc_rt_span *out, size_t max);

#endif /* OC_RICHTEXT_H */
