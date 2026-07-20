# tuikit — the OpenChime TUI toolbox

`tuikit` is an in-tree C library that merges the terminal layer (termbox2 + the Windows
console backend + utf8proc width) with a small set of widgets, a formatting/style layer, and
a keymap/help model. It is the foundation the OpenChime TUI is built from, and it is written
to be **generic and reusable** — nothing in `tuikit` knows about OpenChime, `oc_model`, or
chat. See ARCH-83 for the decision record.

It is **not** a Bubble Tea runtime port. There is no Model/Update/View loop and no async Cmd
system — `tuikit` is a widget library of plain C structs + functions. The app owns the widget
structs and drives them from its own event loop.

## What's in the box

**Terminal layer**
- `tk_term` — merges termbox2 (POSIX single-header) + the Windows Console backend behind one
  API (init/shutdown/present/poll + raw cell ops). Owns the single `TB_IMPL` instantiation.

**Primitives**
- `tk_draw` — width-correct text: `tk_cp_width`, `tk_str_width`, `tk_text` (clipped UTF-8
  blit), `tk_text_right` (right-aligned blit), `tk_fill`. Wraps `utf8proc` so every glyph's
  display width is correct (the thing that otherwise corrupts a whole layout).
- `tk_style` — bordered boxes: `tk_panel` (titled, focus-aware border) and `tk_box` (border +
  cleared interior, the frame under a modal). A full Lip Gloss-style struct (padding/align/
  attrs) is not implemented — helpers grow here as widgets need them.

**Theme / color**
- `tk_theme` — semantic 256-color tokens (`bg`, `surface`, `fg`, `muted`, `faint`, `accent`,
  `accent2`, `sel_bg`, `sel_fg`, `border`, `border_active`, `ok`, `warn`, `err`,
  `presence_on/away/off`, `header_bg`, `footer_bg`). One dark default, returned by
  `tk_theme_active()`; every widget draws through it. See **Color model** below.

**Widgets (the 6)**
- `tk_list` — generic filterable/selectable list (items + a render delegate). Arrow-nav with
  always-live type-to-filter (no `/` gate, no `j/k`). The backbone of sidebars, browsers, menus.
- `tk_input` — single-line text field (placeholder, password echo; end cursor, append/backspace
  only — no left/right movement yet; fixed 512-char buffer).
- `tk_textarea` — multi-line, soft-wrap field (the message composer; append/backspace only).
- `tk_viewport` — scrollable content region (follow-bottom).
- `tk_modal` — centered overlay frame. Draws the box and returns the inner rect; **focus
  capture is the app's job** (it routes events to the modal's widgets while open).
- `tk_palette` — an opencode-style command palette: a borrowed item array
  (`section`/`label`/`desc`/`keyhint`/`id`), always-on incremental search, ↑/↓ select,
  Enter → id, Esc cancels; section headers in `accent`, right-aligned key hints, gray
  descriptions; self-centers as its own modal. Backs the Ctrl+K / `:` action launcher.

**Keymap / help**
- `tk_key` — bindings as data (`tk_binding { key, ch, help_key, help_desc, enabled }`) +
  `tk_key_match()` and a help renderer (`tk_help_footer` / `tk_help_full`). The footer and
  `?` screen are generated from the same bindings that handle the keys, so they can never
  drift — the antidote to hardcoded hint strings.

**Not yet built (grow-on-demand)**
- `tk_spinner`, `tk_progress`, `tk_filepicker` — not present today; added when a screen needs
  them.

## Color model

The toolbox runs in **256-color** mode (`tb_set_output_mode(TB_OUTPUT_256)`) — set by the app
and the demo at startup. Widgets never hardcode palette indices; they draw through the semantic
tokens `tk_theme_active()` returns.

One gotcha is baked in: termbox2's named color constants (`TB_RED`, `TB_GREEN`, …) are
off-by-one in 256-color mode. `tk_theme.h` therefore `#undef`s and **remaps** the eight names
to their true xterm-256 indices, so a named color still looks like its name. Every color-writing
translation unit includes `tk_theme.h` — but `tk_term.c` must **never** include it, because
termbox2's own `TB_IMPL` there relies on the original (unremapped) constants.

## Widget API shape

Every widget is a struct the app owns, with an init / handle-event / draw triad:

```c
typedef struct tk_list tk_list;
void      tk_list_init(tk_list *l, const tk_list_opts *o);         /* delegate + count fn */
tk_result tk_list_handle(tk_list *l, const struct tb_event *ev);   /* TK_NONE/USED/SELECT/CANCEL/CHANGED */
void      tk_list_draw(tk_list *l, tk_rect r);
int       tk_list_selected(tk_list *l);
```

Shared types: `tk_rect { int x, y, w, h }`; events are `struct tb_event`; results are a small
`tk_result` enum. Widgets render generic data through a **delegate callback** — the app
supplies a function that formats item *i* into a styled string. This is the rule that keeps
`tuikit` free of app types: e.g. the chat channel sidebar is a `tk_list` whose delegate reads
`oc_model`, but `tk_list` itself never sees `oc_model`.

## Dependencies & boundaries

- `tuikit` depends **only** on `third_party/termbox2` and `third_party/utf8proc`.
- It must **never** include `client/core`, `oc_model`, `oc_client`, or any OpenChime header.
- Chat-specific code stays in the app (`client/tui/`): the message/roster/search row builders
  (reimplemented as `tk_list`/`tk_viewport` delegates), the panel `layout`, `ws_session`,
  `config.c`, `secret_backend.c`.

## Build

Compiled into the TUI targets (repo style — one gcc invocation, no separate archive step):
`TUIKIT_SRC := $(filter-out tuikit/demo.c,$(wildcard tuikit/*.c))` + `-Ituikit`, added to both
`tui` and `windows-tui` (`demo.c` is filtered out — it carries its own `main()`).
`tk_term` absorbs the former `client/tui/termbox2_win.c`. `termbox2.h`/`utf8proc` stay
vendored in `third_party/`. `libtuikit.a` (an `ar` archive of `tuikit/*.o`) is a documented
option for reuse elsewhere, not required now.

`tuikit/demo.c` (`make tuikit-demo`) exercises each widget standalone — the library's own
smoke, no daemon needed.

## Build order (shipped)

The toolbox and the redesign it enables are complete. It landed in this order:

1. **Terminal merge + `tk_draw`/`tk_style`** — zero behaviour change; the TUI runs identically
   on POSIX and Windows. De-risked the library before any widget work.
2. **`tk_key`** — the keymap/help model; converted the TUI's hint bar + help screen to it.
3. **The widgets** — `tk_list`, `tk_input`, `tk_textarea`, `tk_viewport`, `tk_modal`, with
   `tuikit/demo.c`.
4. **Migrated the chat TUI onto the widgets**, then **deleted the slash-command dispatcher** —
   the composer's Enter only sends; actions moved to context menus, dialogs, and the Ctrl+K /
   `:` command palette. This menu/screen-driven UX is the reason `tuikit` exists.
5. **Round 2** — 256-color output + `tk_theme`, the `tk_palette` command launcher, arrow-nav
   (dropped `j/k`), and on-screen key hints in `Ctrl+X` form.

The reuse map (which former `client/tui/main.c` helper seeded which module): `cp_width`/
`draw_clip`/`fill_row` → `tk_draw`; `draw_panel`/`draw_frame` → `tk_style`; `fuzzy`/
`palette_build`/pickers → `tk_list`+`tk_modal`+`tk_palette`; `draw_field`/composer edit →
`tk_input`; `wrap_push` → `tk_textarea`/`tk_viewport`; the hint strings + `draw_help` → `tk_key`.
