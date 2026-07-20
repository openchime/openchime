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
  blit), `tk_fill`. Wraps `utf8proc` so every glyph's display width is correct (the thing
  that otherwise corrupts a whole layout).
- `tk_style` — the Lip Gloss subset: a style struct (fg/bg/attrs/padding/border/align) +
  `tk_box`/`tk_panel`. Borders/padding/alignment over cells (termbox emits the ANSI).

**Widgets (the 5)**
- `tk_list` — generic filterable/selectable list (items + a render delegate). The backbone of
  sidebars, browsers, menus, and the action palette.
- `tk_input` — single-line text field (cursor, placeholder, char limit, password echo).
- `tk_textarea` — multi-line, soft-wrap field (the message composer).
- `tk_viewport` — scrollable content region.
- `tk_modal` — centered overlay + focus capture (container for menus, forms, confirms).

**Keymap / help**
- `tk_key` — bindings as data (`tk_binding { key, ch, help_key, help_desc, enabled }`) +
  `tk_key_match()` and a help renderer (`tk_help_footer` / `tk_help_full`). The footer and
  `?` screen are generated from the same bindings that handle the keys, so they can never
  drift — the antidote to hardcoded hint strings.

**Optional (grow-on-demand)**
- `tk_spinner`, `tk_progress`, `tk_filepicker` — added when a screen needs them.

## Widget API shape

Every widget is a struct the app owns, with an init / handle-event / draw triad:

```c
typedef struct tk_list tk_list;
void      tk_list_init(tk_list *l, const tk_list_opts *o);         /* delegate + count fn */
tk_result tk_list_handle(tk_list *l, const struct tb_event *ev);   /* TK_SELECT/TK_CANCEL/TK_NONE */
void      tk_list_draw(tk_list *l, tk_rect r);
int       tk_list_selected(const tk_list *l);
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
`TUIKIT_SRC := $(wildcard tuikit/*.c)` + `-Ituikit`, added to both `tui` and `windows-tui`.
`tk_term` absorbs the former `client/tui/termbox2_win.c`. `termbox2.h`/`utf8proc` stay
vendored in `third_party/`. `libtuikit.a` (an `ar` archive of `tuikit/*.o`) is a documented
option for reuse elsewhere, not required now.

`tuikit/demo.c` (`make tuikit-demo`) exercises each widget standalone — the library's own
smoke, no daemon needed.

## Build order

1. **Terminal merge + `tk_draw`/`tk_style`** — zero behaviour change; the TUI runs identically
   on POSIX and Windows. De-risks the library before any widget work.
2. **`tk_key`** — the keymap/help model; converts the TUI's hint bar + help screen to it.
3. **The 5 widgets** — `tk_list`, `tk_input`, `tk_textarea`, `tk_viewport`, `tk_modal`, with
   `tuikit/demo.c`.
4. **Migrate the chat TUI onto the widgets** — behaviour preserved (still command-capable).
5. *(separate effort)* the menu/screen-driven UX redesign that replaces slash commands — the
   reason `tuikit` exists.

The reuse map (which existing `client/tui/main.c` helper seeds which module) lives in the
implementation plan; the short version: `cp_width`/`draw_clip`/`fill_row` → `tk_draw`;
`draw_panel`/`draw_frame` → `tk_style`; `fuzzy`/`palette_build`/pickers → `tk_list`+`tk_modal`;
`draw_field`/composer edit → `tk_input`; `wrap_push` → `tk_textarea`/`tk_viewport`; the hint
strings + `draw_help` → `tk_key`.
