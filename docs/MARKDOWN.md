# OpenChime — Message Formatting (REQ-220, ARCH-100)

The markup a message body may contain, how it is parsed, and where. This is the
contract every client renders against; it is not "some Markdown".

**Summary.** A **Slack-compatible subset** for inline emphasis, extended with
real list syntax, parsed **client-side in `shared/richtext.c`** and never
rendered by the daemon. The stored body stays **plain UTF-8** (REQ-054) with the markup in band —
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
| Link | `https://example.com` | Not typed as markup: a bare `http`/`https` address is **autolinked**. |
| Labelled link | `[label](https://example.com)` | Text that links to an address. The reader always sees the address before it opens — see §4. |
| Table | `\| a \| b \|` over `\|---\|---\|` | A pipe table, as GitHub writes it — see §1a. |

Emphasis nests (`*bold with _italic_ inside*`); code does not — everything inside
a code span or block is literal, including other delimiters.

## 1a. Tables

A table is GitHub's pipe table:

```
| Region | Q1  | Notes                    |
|--------|----:|:------------------------:|
| East   | 120 | *up*<br>since May         |
| West   |  90 | see [the sheet](https://example.com/q1) |
```

- **A header row, then a separator row, then body rows.** The separator's cells
  are dashes, with an optional colon at either end for the column's alignment:
  `:---` left, `:---:` centre, `---:` right, `---` the default (left). The
  header and the separator must have the same number of cells; a body row with
  fewer has empty ones, and extra cells are ignored, as GitHub does.
- **Pipes.** Leading and trailing pipes are optional. `\|` is a literal pipe in
  a cell — the escape works anywhere, but a table is where it is needed.
- **Cells are inline text.** Emphasis, code, links, labelled links, emoji,
  custom emoji, @mentions and #channels all work in a cell; block markers do
  not, and no inline construct reaches from one cell into the next.
- **`<br>` is a line break in a cell** (`<br/>` and `<br />` too, in any case).
  It is GitHub's convention and the only piece of HTML the dialect accepts —
  only in a cell, because a cell has no other way to hold a second line.
- **No header** is written as a header row of empty cells; it draws as a table
  without one.
- **Where a table ends.** At a blank line, or the first line with no pipe.
  GitHub carries such a line on as one more row; in a chat message the sentence
  after a table is a sentence, so here it is.
- **Where one cannot start.** Inside a fenced code block, or on a line that
  begins with a block marker (`>`, `- `, `1. `): a table is not part of a quote
  or a list. A header row must contain a pipe, so a lone `---` under a line of
  text is never a table.
- **At most 20 columns.** Rows are bounded only by the body (REQ-054).

Anything that does not meet those rules is not a table and is scanned as text,
so a malformed table shows exactly as typed (§2's last rule). A body stores a
table as this Markdown and nothing else: the wire, the schema and search are
unchanged, and a table typed by hand, pasted from a sheet or built in the table
editor is the same thing once sent.

The parser reports a table as one block span (`OC_RT_TABLE`) and runs no inline
markup across it; its rows and cells come from `oc_rt_tables` and the row
iterator, and a cell's inline spans from `oc_rt_scan_cell`, all with offsets
into the unchanged body. Wherever a message is reduced to one line — a sidebar
preview, a toast, a search hit, a screen reader's name for a row — a table reads
**"Table: N rows × M columns"** (`oc_rt_flatten`).

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
3. **Backslash escapes.** `\*` `\_` `\~` `` \` `` `\>` `\-` `\|` and `\\` produce the
   literal character. Slack has no escape at all; this is a deliberate addition,
   because without one there is no way to write a literal asterisk at a word
   boundary and the answer "you cannot" is not one.

Two clarifications the implementation forced, recorded because they are the
difference between the rules above working and merely sounding right:

- **A run of delimiters is looked *through* to find the boundary.** Rule 1 has
  to accept a delimiter as an opener's left-hand context or `*_x_*` could never
  nest, but accepting it blindly makes `snake__case__here` italicise `case_` —
  the run traces back to `e`, and an identifier is not a word boundary however
  many underscores are in it.
- **Only `**` means anything doubled.** `~~struck~~`, `__loud__` and `***x***`
  are literal text. The alternative is matching the first delimiter of the pair
  and striking through `~struck` — visibly half-eaten, which is worse than
  leaving what the author typed alone.

An unmatched or ambiguous construct **always degrades to its literal source**.
Rendering is never allowed to lose characters the author typed.

## 3. Where it is parsed

**`shared/richtext.[ch]`, and nowhere else.**

- **Rendering stays in the clients.** Formatting needs no server knowledge to
  display, which is where it differs from @mentions (ARCH-89), which *had* to
  resolve server-side because only the daemon holds the roster. The wire carries
  the author's bytes, never markup, and no frame describes formatting.
- **In `shared/` because the daemon reads messages aloud** (ARCH-111). Turning a
  body into speakable text (`shared/speakable.c`) has to skip code blocks and drop
  delimiters by exactly the rules a reader sees applied, so the daemon links the
  same parser the GUI does.
- **One parser, one renderer.** The GUI — Windows and the web alike — calls it
  and receives the spans, and the daemon's speakable text uses the same function,
  for the reason ARCH-89 gives for the mention scanner: two implementations of
  "is this bold" will drift, and nobody can tell which is right from either side
  alone. The TUI does not parse a message body (nothing in `client/tui/`
  includes `richtext.h`); it shows one as written, markers included. Its
  action (`/me`) lines and search hits go through the core's `oc_model_line`,
  which flattens a table with `oc_rt_flatten` (§1a).

The parser returns **spans over the original bytes** — `{start, len, style}` —
never a rewritten string. The body a client renders is byte-identical to the body
the daemon stored, which keeps search (FTS5 over the raw body), mention offsets
(migration 0021's byte spans) and message editing all addressing the same text.

**A leading `/me` is not markup.** A body that begins `/me`, one or more spaces,
then text is an **action** (REQ-058, ARCH-115): the daemon decides it, records it
beside the body and tells every client, which shows the actor's name and then the
action text — `/me is away` reads "Ada Starr is away". The parser above runs over
the action text only, never over the name in front of it, so the author's own
formatting applies exactly as it would in any message and a name is never styled
as if it were markup.

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
user typing `<` in ordinary prose would see it mangled. So `&`, `<` and `>` are
ordinary characters and a URL is ordinary text.

**A URL needs no construct — it autolinks.** A bare `http://` or `https://`
address yields one `OC_RT_LINK` span over exactly the address, with **no
delimiter span**, because there is no markup around it to hide: the address is
its own label. Slack's `<url|label>` is declined above.

**A labelled link is Markdown's `[label](url)`, and it never opens unseen.**
The label is one line, non-empty, with no brackets of its own, and may carry
emphasis; the address follows every autolink rule — `http` or `https` only, the
same end — and is closed by the `)`. It yields an `OC_RT_LABELLED` span over the
label and delimiter spans over the `[` and the `](address)`; `oc_rt_target` hands
a frontend the address. Anything short of that is literal text, in which a bare
address still autolinks. Because a label can say anything while the link points
elsewhere — the shape every phishing message wants — the address stays the
trustworthy part: a frontend shows it on hover and asks before opening a
labelled link, naming its host and the full address (§6). A bare address needs
no such step; it is its own label.

**Only `http` and `https`.** A link span is what a frontend hands to the
operating system, so this list is the set of things a message can ask a reader's
machine to open. `file://`, `javascript:`, `mailto:` and anything else stay
ordinary text. The restriction lives in the shared parser rather than in each
frontend, which is what stops two clients answering a security question
differently — the same argument ARCH-89 makes for the mention scanner.

**Where an address ends** is the part with all the edge cases, and the rule is
that trailing punctuation belongs to the sentence: `see https://example.com.`
links the address and not the full stop. A closing bracket counts only if the
address opened it, so `(see https://example.com/a)` drops the `)` while
`https://en.wikipedia.org/wiki/Foo_(bar)` keeps it. `*` and `~` are trimmed too,
so `*https://example.com*` is a bold link — but **`_` deliberately is not**,
because underscores are ordinary inside real addresses and asterisks are not.
The consequence is recorded rather than hidden: `_https://example.com_` links
`https://example.com_` and is not italic. Trimming a character the address owns
changes where the link goes, silently, which is a worse failure than an italic
that does not render.

Emphasis never reaches inside an address either — the closer search steps over a
URL exactly as it steps over a code span — so an `_` in a path cannot end an
italic run that opened before it.

REQ-222's **unfurl** (fetching a title and description for a link) is a
different feature with its own design (ARCH-105): the daemon fetches
server-side, using this same shared address scanner so it unfurls exactly what
a client links. It is not a property of this dialect.

**No underline.** Slack's toolbar offers it; its `mrkdwn` has no syntax for it,
and an underline is indistinguishable from a link in most renderings.

## 5. Non-goals

- **No WYSIWYG-only constructs.** Anything the toolbar can produce must be
  expressible in text, or the two authoring paths diverge and a message becomes
  uneditable in one of them. (The toolbar, and `Ctrl+B`/`Ctrl+I`, insert
  delimiters into the plain body and nothing else, so the constraint
  holds by construction rather than by discipline: there is no second
  representation for the two paths to disagree about.)
- **No headings, images or HTML** (beyond `<br>` in a table cell, §1a). A chat
  message is not a document: headings in a 400-character message are noise, and
  images are attachments (REQ-140/142). Tables were on this list, on the grounds
  that they need a column model no terminal can honour. They are not any more:
  tabular figures are an everyday chat payload, every alternative to a table is
  misaligned text, and the terminal's answer is the one it gives every
  construct — it shows the source, which for a pipe table is a readable grid.
- **No link *titles*, reference-style links, or images.** Titles and references
  exist to make long-form prose readable and neither survives a chat transcript;
  an image is an attachment.
- **The TUI shows the source, not the formatting.** It does not parse the body:
  bold, italic, code, links and tables appear with their delimiters, as typed
  (in an action line or a search hit a table is flattened to its one-line
  summary, §1a). That stays
  readable because the delimiters are the ones people already type in plain
  text, and the stored body is the same either way.

## 6. Rendering

Each frontend maps spans to its own facilities, and the mapping is the frontend's
business:

- **Tables** (§1a). The GUI draws a grid: each cell laid out with its own spans,
  columns as wide as their content up to a limit and wrapped past it, the header
  row shaded and bold, each column aligned as its separator says, and a table
  wider than the message column scrolls sideways on its own. A link in a cell
  opens as any link does, and "Copy table" puts the table on the clipboard as
  tab-separated text that pastes back into a spreadsheet. In the **composer** a
  table is a compact card with Edit and Remove: the editor's buffer holds one
  placeholder character standing for the table's Markdown, drawn as the card, so
  the caret, deletion and undo treat it as one unit and it keeps its place among
  the text around it; the Markdown replaces it on send, on a draft's save and on
  copy. That is a second representation, but only of a whole construct and only
  while editing — never of part of one, which is what §5's first non-goal guards
  against. A table typed as Markdown stays text in the composer until it is sent.
  People make tables without the syntax in two ways: **Insert table** on the
  formatting toolbar opens a table editor (a column count, then the cells row by
  row, with alignment per column and the header row optional), and pasting cells
  copied from a spreadsheet offers **Table**, **Image** or **Text**. Image is the
  clipboard's own picture of the cells when it holds one, as Excel's does; a
  sheet in a browser puts none there, so otherwise the cells are drawn by the
  same grid the transcript uses and attached as a PNG. **Read aloud**, a table is announced —
  "Table, 3 rows." — and its cells are skipped. The **TUI** shows the source.

- **GUI** (Windows and the web) — sdltext ranges (DirectWrite's on Windows) on
  the existing layout, the same mechanism `@mention` highlighting already uses
  (`body_layout`), so formatting composes with mentions and custom emoji rather
  than fighting them. **Both the transcript and the composer remove the inline
  delimiters**, so a body is drawn the same way wherever it appears —
  you see formatting, never markup, as in Slack. Removing one collapses
  it (`st_range_hide`, a 0.1 DIP size under DirectWrite) rather than making it
  transparent, because the layout still covers the raw body and a transparent
  asterisk keeps its width — two gaps around every
  bold word, which read as typos. Block markers (`- `, `1. `, `> `) stay visible
  but faint: in a range-only treatment they *are* the rendering of a list or a
  quote.

  A preference chooses between them: *Rich text*, which is the
  default and is described here, or *Plain text*, where the markup is shown as
  written and the field never restyles or rewrites it. Two audiences, one of
  whom types this dialect fluently and does not want an editor second-guessing
  it; Slack offers the same switch.

  In rich mode the composer keeps the plain source in its buffer — there is still no second
  representation — and pays for the hidden characters in caret arithmetic
  instead: every caret position canonicalises to the leftmost of the set that
  draws in the same place, arrows cross an invisible run within one keypress,
  typed text lands inside an adjacent run rather than breaking it, and a
  delimiter that stops parsing because of an edit elsewhere is **deleted** — the
  formatting goes, the markup never appears. Two pieces of **typing intent**
  carry the rich-editor contract over the places the dialect cannot express
  directly: a style toggled with nothing to wrap is *pending* (nothing inserted
  until the first character, so an empty pair never sits visible), and
  whitespace typed at a run's back edge steps *outside* the closer and arms a
  *continuation* that pulls the closer forward over the gap at the next word —
  so "bold on, type a sentence" stays bold across its spaces. A toggle also
  snaps a mid-word edge outward to the word, absorbs same-style runs the range
  touches, and splits an enclosing run to un-format a piece of it; every one of
  those exists so a toggle can never leave markup that does not parse. That last rule is forced: keeping
  `x` emphasised in `a*x*` would need the word-boundary rule of §2 to bend, and
  that rule is what stops `a_variable_name` becoming italic.
  **A link is accent + underline, and the cursor is the affordance**.
  Colour alone is not one — it is what a `@mention` already wears, and it says
  nothing to a reader who cannot distinguish it — so the underline carries "this
  goes somewhere" and the hand cursor confirms it before the click. Opening
  happens on **release over the same address the press landed on**, not on press,
  so dragging a selection that starts inside a URL still selects rather than
  launching a browser; sliding off before letting go cancels, as everywhere else.
  `link_open` re-checks the scheme before `SDL_OpenURL` even though the parser
  already guaranteed it, because that call is the dangerous end. **A labelled link** wears the same
  accent and underline over its label, with the `](address)` hidden in the
  transcript but shown faint in the composer, so its author sees where it goes.
  Hovering one shows its real address in a tip beside the pointer, and a click
  opens a confirmation — "This link goes to *host*", the full address, and Open
  link — rather than the browser.
- **TUI** — no rendering: the body is drawn as written, delimiters and all,
  since the TUI never parses a body (action lines and search hits flatten a
  table, through the core's `oc_rt_flatten`). Markup shown as source stays legible,
  which the dialect guarantees.

**The composer shows formatting as you type**, which is only affordable because
the parser is client-side and runs over a ≤4000-unit buffer — the same pass that
already re-scans mentions on every keystroke.
