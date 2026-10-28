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
  cleared interior, the frame under a modal). There is deliberately no full Lip Gloss-style struct (padding/align/
  attrs) — helpers grow here as widgets need them.
- `tk_qr` — a QR code as terminal text: `tk_qr_render` gives the rows, two modules to a cell
  in half blocks, with a quiet zone (the device code's URL, AUTH.md §8.11). Pure, no terminal;
  the encoding is `third_party/qrcodegen`.

**Theme / color**
- `tk_theme` — semantic 256-color tokens (`bg`, `surface`, `fg`, `muted`, `faint`, `accent`,
  `accent2`, `sel_bg`, `sel_fg`, `border`, `border_active`, `ok`, `warn`, `err`,
  `presence_on/away/off`, `header_bg`, `footer_bg`). One dark default, returned by
  `tk_theme_active()`; every widget draws through it. See **Color model** below.

**Widgets (the 6)**
- `tk_list` — generic filterable/selectable list (items + a render delegate). Arrow-nav with
  always-live type-to-filter (no `/` gate, no `j/k`). The backbone of the TUI's action menus.
- `tk_input` — single-line text field (placeholder, password echo; end cursor, append/backspace
  only — no left/right movement; fixed 512-byte UTF-8 buffer).
- `tk_textarea` — multi-line, soft-wrap field (append/backspace only; not yet used by the TUI).
- `tk_viewport` — scrollable content region (follow-bottom); not yet used by the TUI.
- `tk_modal` — centered overlay frame. Draws the box and returns the inner rect; **focus
  capture is the app's job** (it routes events to the modal's widgets while open).
- `tk_palette` — an opencode-style command palette: a borrowed item array
  (`section`/`label`/`desc`/`keyhint`/`id`), always-on incremental search, ↑/↓ select,
  Enter → id, Esc cancels; section headers in `accent`, right-aligned key hints, gray
  descriptions; self-centers as its own modal. Backs the Ctrl+K / `:` action launcher.

**Keymap / help**
- `tk_key` — bindings as data (`tk_binding { key, ch, help_key, help_desc, enabled }`) +
  `tk_key_match()` and a help renderer (`tk_help_footer` / `tk_help_full`), so a screen that
  matches keys with `tk_key_match` gets hints that cannot drift from them. The TUI today uses
  only `tk_help_footer`; its key handling and `?` screen are still hand-written.

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
`tuikit` free of app types: e.g. the TUI's action menu is a `tk_list` whose delegate
(`menu_text`) reads the app's menu array, but `tk_list` itself never sees it.

## Dependencies & boundaries

- `tuikit` depends **only** on `third_party/termbox2`, `third_party/utf8proc` and
  `third_party/qrcodegen` (for `tk_qr`).
- It must **never** include `client/core`, `oc_model`, `oc_client`, or any OpenChime header.
- Chat-specific code stays in the app (`client/tui/main.c`: the message/roster/search row
  builders, `layout()`, the `ws_session` table; and `config.c`). The credential backends are **not** among them — they live in
  `client/shared/` (`secret_libsecret.c`, `secret_win.c`) behind the `oc_secret`
  vtable (`client/core/secret.h`), because both frontends use them.

## Build

Compiled into the TUI targets (repo style — one gcc invocation, no separate archive step):
`TUIKIT_SRC := $(filter-out tuikit/demo.c,$(wildcard tuikit/*.c)) $(QRCODEGEN)` +
`TUIKIT_INC` (`-Ituikit -Ithird_party/qrcodegen`), added to both
`linux` and `windows` (`demo.c` is filtered out — it carries its own `main()`).
`tk_term` owns the Windows console backend. `termbox2.h`/`utf8proc`/`qrcodegen` stay
vendored in `third_party/`. (An `ar` archive of `tuikit/*.o` would make it reusable
elsewhere; no target builds one.)

`tuikit/demo.c` (`make build/tuikit-demo`) exercises each widget standalone — the library's own
smoke, no daemon needed.
