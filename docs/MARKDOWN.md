# OpenChime — Message Formatting (REQ-220, ARCH-100)

The markup a message body may contain, how it is parsed, and where. This is the
contract every client renders against; it is not "some Markdown".

**Summary.** A **Slack-compatible subset** for inline emphasis, extended with
real list syntax, parsed **client-side in `client/core/`** and never by the
daemon. The stored body stays **plain UTF-8** (REQ-054) with the markup in band —
no schema change, no second representation, and a client that cannot render a
construct shows its source legibly.

---

## 1. The syntax

| Construct | Syntax | Notes |
|---|---|---|
| Bold | `*bold*` | A **single** asterisk, as Slack. `**bold**` is accepted too — see §4. |
| Italic | `_italic_` | |
| Strikethrough | `~struck~` | |
| Inline code | `` `code` `` | Suppresses all other markup inside it. |
| Code block | ` ```…``` ` | Fenced; may span lines. Suppresses everything inside. |
| Blockquote | `> quoted` | At the start of a line. |
| Bulleted list | `- item` | At the start of a line. **Not Slack** — see §4. |
| Ordered list | `1. item` | At the start of a line. **Not Slack** — see §4. |

Emphasis nests (`*bold with _italic_ inside*`); code does not — everything inside
a code span or block is literal, including other delimiters.

## 2. When a delimiter is *not* markup

The single most common failure of an in-band dialect is eating text somebody
meant literally. Three rules, in order:

1. **Word-boundary anchoring.** An opening delimiter must be preceded by
   whitespace, a line start, or an opening bracket, and **followed by a
   non-space**. A closing delimiter must be preceded by a non-space. So
   `2 * 3 * 4` is arithmetic, `a_variable_name` is an identifier, and
   `*emphasis*` is emphasis.
2. **It must close on the same line** (except a fenced block, which may not).
   An unclosed delimiter is literal text — a half-typed `*` never restyles the
   rest of the message.
3. **Backslash escapes.** `\*` `\_` `\~` `` \` `` `\>` `\-` and `\\` produce the
   literal character. Slack has no escape at all; this is a deliberate addition,
   because without one there is no way to write a literal asterisk at a word
   boundary and the answer "you cannot" is not one.

An unmatched or ambiguous construct **always degrades to its literal source**.
Rendering is never allowed to lose characters the author typed.

## 3. Where it is parsed

**`client/core/richtext.[ch]`, and nowhere else.**

- **Not the daemon.** Formatting needs no server knowledge. This is the point
  where it differs from @mentions (ARCH-89), which *had* to resolve server-side
  because only the daemon holds the roster. Parsing markup server-side would buy
  nothing and add a wire contract to version forever.
- **Not `shared/`.** That directory is the **wire contract shared with the
  daemon** — `protocol.c`, `mention.c`, `searchq.c` are all there because the
  daemon links them too. Formatting is shared between *frontends only*, which is
  exactly what `client/core/` is (see `complete.c`, the shared completion and
  emoji catalogue).
- **One parser, both frontends.** The TUI and the GUI call the same function and
  receive the same spans, for the reason ARCH-89 gives for the mention scanner:
  two implementations of "is this bold" will drift, and nobody can tell which is
  right from either side alone.

The parser returns **spans over the original bytes** — `{start, len, style}` —
never a rewritten string. The body a client renders is byte-identical to the body
the daemon stored, which keeps search (FTS5 over the raw body), mention offsets
(migration 0021's byte spans) and message editing all addressing the same text.

## 4. Where we deliberately differ from Slack

Recorded so the divergences are choices rather than drift.

**Lists are ours.** Slack's `mrkdwn` has **no list syntax** — its documentation
says to "mimic list formatting with regular text and line breaks", and its
toolbar produces lists that the API cannot express. REQ-220 asks for real ordered
and unordered lists, so we take the standard Markdown forms (`- ` and `1. `).
This is a superset, not an incompatibility: text written for Slack renders the
same here.

**`**bold**` is also bold.** Slack takes a single `*`. Everyone arriving from
Markdown, GitHub or almost anywhere else types two. Rendering `**x**` as literal
asterisks would look broken to more people than it would please, so both forms
produce bold and the canonical form in our own docs is the single asterisk.

**No `<URL|label>` links, and no HTML entity escaping.** Slack requires `&`, `<`
and `>` to be sent as `&amp;`, `&lt;`, `&gt;`, and wraps links as
`<https://example.com|text>`. Both are artifacts of Slack's *API* layer, and
adopting them would be actively harmful here: our bodies are plain UTF-8
(REQ-054) that FTS5 indexes directly, so entity-encoding would put `&amp;` into
the search index and into every client that renders the body literally, and a
user typing `<` in ordinary prose would see it mangled. URLs are detected by
**autolinking** instead (REQ-222's unfurl already works from the bare URL), and
`&`, `<`, `>` are ordinary characters.

**No underline.** Slack's toolbar offers it; its `mrkdwn` has no syntax for it,
and an underline is indistinguishable from a link in most renderings.

## 5. Non-goals

- **No WYSIWYG-only constructs.** Anything the toolbar can produce must be
  expressible in text, or the two authoring paths diverge and a message becomes
  uneditable in one of them. (The toolbar itself, and `Ctrl+B`/`Ctrl+I`, are
  WIN-96 — a second way to author what you can already type, and worth nothing
  until the parser exists.)
- **No tables, headings, images or HTML.** A chat message is a paragraph, not a
  document. Headings in a 400-character message are noise; tables need a column
  model no terminal can honour; images are attachments (REQ-140/142).
- **No link *titles* or reference-style links.** Both exist to make long-form
  prose readable and neither survives a chat transcript.
- **The TUI renders the same structure without proportional styling** (REQ-220):
  bold and italic become terminal attributes, code blocks are shown in band. It
  is not exempt from formatting the way it is exempt from images (ARCH-75).

## 6. Rendering

Each frontend maps spans to its own facilities, and the mapping is the frontend's
business:

- **Win32** — DirectWrite ranges on the existing layout, the same mechanism
  `@mention` highlighting already uses (`body_layout`), so formatting composes
  with mentions and custom emoji rather than fighting them.
- **TUI** — tuikit attributes; code blocks and blockquotes get in-band markers
  since a terminal has no proportional styling to lean on.

**The composer shows formatting as you type**, which is only affordable because
the parser is client-side and runs over a ≤4000-unit buffer — the same pass that
already re-scans mentions on every keystroke.
