/*
 * OpenChime Win32 GUI — theme palette (ARCH-82, REQ-262).
 *
 * The palette is a runtime array rather than a set of literals, so a theme can
 * be switched while the app runs. Call sites are unchanged: each
 * OC_COL_* still reads as a plain 0xRRGGBB expression, it just resolves through
 * `oc_theme[]` now. Nothing may use one in a static initialiser for that reason.
 *
 * Colors are 0xRRGGBB; col()/oc_d2d_color() turn them into D2D1_COLOR_F.
 */
#ifndef OC_GUI_THEME_H
#define OC_GUI_THEME_H

#include <stdint.h>

enum {
    TH_ACCENT = 0,   /* selection, unread dot, links, focus, send button */
    TH_ACCENT_DIM,
    TH_RAIL,         /* shell neutrals, darkest rail -> lightest canvas */
    TH_SIDEBAR,
    TH_BASE,
    TH_HEADER,
    TH_INPUT,
    TH_SELECT,
    TH_HOVER,
    TH_BORDER,
    TH_RAIL_ICON,    /* rail icon/label when unselected */
    TH_TEXT,
    TH_MUTED,
    TH_FAINT,
    TH_DANGER,
    TH_NOTICE,       /* transient notices — see the note below */
    TH_ONLINE,
    TH_AWAY,
    TH_ON_ACCENT,    /* a label or icon drawn ON an accent, accent-dim or danger fill */
    TH_COUNT
};

/* Theme modes, in the order the preferences UI offers them. */
/* SYSTEM is the DEFAULT (2026-07-30). An app that ignores the desktop's own
 * light/dark setting looks like it was written before the setting existed —
 * matching it is the least surprising thing a native client can do, and it is the
 * reason ARCH-82 chose native rendering in the first place. Dark and light stay as
 * explicit overrides for people who want one regardless of the machine. */
enum { OC_THEME_DARK = 0, OC_THEME_LIGHT = 1, OC_THEME_SYSTEM = 2 };

extern uint32_t oc_theme[TH_COUNT];

/* COLOUR SCHEMES. A scheme is a PAIR: the nav rail — the app's main
 * branded surface, the thing you see before you read anything — and the accent
 * that marks selection, links and primary buttons. They are picked together
 * because they are seen together; choosing an accent that fights the rail was
 * possible when the rail was fixed, and looked like a bug rather than a choice.
 *
 * Each scheme carries BOTH colours for BOTH modes. The bright blue that pops on
 * navy is unreadable on white, which is the whole reason the two palettes differ
 * in their accent at all — picking a scheme must not undo that.
 *
 * A fixed set rather than a colour picker: every value has to stay legible as
 * white-on-accent on a primary button and as white icons on the rail, and an
 * arbitrary RGB cannot promise either. */
enum { OC_SCHEME_MIDNIGHT = 0, OC_SCHEME_INDIGO, OC_SCHEME_TEAL, OC_SCHEME_PLUM,
       OC_SCHEME_COUNT };

void oc_theme_set_scheme(int scheme);
int  oc_theme_scheme(void);
const char *oc_theme_scheme_name(int scheme);
/* The two swatches to DRAW for `scheme`, resolved for the mode in force — so the
 * chooser shows the pair you will actually get. */
uint32_t oc_theme_scheme_accent(int scheme);
uint32_t oc_theme_scheme_rail(int scheme);

/* Where OC_THEME_SYSTEM asks the desktop: the platform's oc_plat_prefers_light,
 * handed in by the application at start (1 light, 0 dark, -1 cannot say). Until
 * then, and whenever it answers anything but 1, SYSTEM resolves to dark. */
void oc_theme_set_system_query(int (*prefers_light)(void));
/* Apply a mode. OC_THEME_SYSTEM follows the desktop's light-or-dark preference,
 * resolving to dark when it cannot be read. */
void oc_theme_apply(int mode);
/* Is the palette in force the LIGHT one? Not the same question as the mode: SYSTEM
 * resolves to either. Anything outside theme.c that has to match the shell — the
 * window caption, for one — asks this rather than asking the desktop again. */
int  oc_theme_is_light(void);
int  oc_theme_mode(void);            /* the mode last applied (not the resolved one) */
const char *oc_theme_mode_name(int mode);

/* --- high contrast (REQ-262) -----------------------------------------------
 *
 * A switch beside the mode, not a third mode: light or dark still follows the
 * Theme row. While it is on, the palette is one of two fixed high-contrast
 * palettes, one per mode, and the colour scheme does not apply -- not the rail,
 * not the accent, not the selection tint derived from it. The scheme chosen is
 * kept and comes back when it goes off. In both palettes every text ink clears
 * 7:1 and every control ink 4.5:1 against every surface it is drawn on, which
 * tests/test_theme.c asserts from the palette.
 *
 * The preference's values, in the order its row shows them. Follow system is
 * the default and is on when the system says so: Windows high contrast, a
 * browser's prefers-contrast: more. */
enum { OC_HC_OFF = 0, OC_HC_ON = 1, OC_HC_SYSTEM = 2, OC_HC_PREF_COUNT };

/* Whether high contrast is on: On is on, Off is off, and Follow system is on
 * only when the system says so (`sys` 1); 0 and -1 (unknown) are off. A value
 * outside the three reads as Follow system. */
int  oc_high_contrast_on(int pref, int sys);

/* Turn the high-contrast palette on or off and re-apply the mode in force. */
void oc_theme_set_high_contrast(int on);
int  oc_theme_high_contrast(void);

/* CORNER RADII, in DIPs.
 *
 * Windows 11 names three and only three. 8 for anything that floats above the
 * content -- windows, dialogs, flyouts, menus, popovers. 4 for anything living
 * inside a surface -- buttons, boxes, chips, list backplates. 0 where an edge
 * meets another straight edge. The system exposes the first two as
 * `OverlayCornerRadius` and `ControlCornerRadius`, and they scale with the
 * display, which is why these are DIPs and not pixels.
 *
 * They live HERE, beside the colours, rather than in oc_gfx. The drawing layer
 * takes a radius as an argument and should hold no opinion about it: the right
 * number is a platform convention, not a property of a rounded rectangle. A
 * frontend under another desktop's conventions supplies its own set without
 * touching a primitive.
 *
 * Two more that Windows does not name but the app needs:
 *
 * OC_R_PILL is a capsule -- the fully-rounded end on unread badges, scrollbar
 * thumbs and the thin indicator bars. It is deliberately larger than any shape
 * it is used on, because gfx_fill_round clamps a radius to half the shorter
 * side: "larger than possible" IS "as round as it goes", and the call site
 * does not have to restate a height it was already handed. That is what the
 * literals it replaces were doing by hand, and getting wrong whenever the
 * height moved.
 *
 * OC_R_AVATAR_LG is the rounded square standing in for a workspace. A PERSON's
 * tile has no token: its corner is a fraction of the tile (avatar_corner() in
 * winmain.c), because a 36px tile and an 18px one with the same radius do not
 * read as the same shape. */
#define OC_R_OVERLAY     8.0f
#define OC_R_CONTROL     4.0f
#define OC_R_PILL      999.0f
#define OC_R_AVATAR_LG  12.0f   /* the 36px workspace tile: rail, menu header, sign-in mark */

#define OC_COL_ACCENT      oc_theme[TH_ACCENT]
#define OC_COL_ACCENT_DIM  oc_theme[TH_ACCENT_DIM]
#define OC_COL_RAIL        oc_theme[TH_RAIL]
#define OC_COL_SIDEBAR     oc_theme[TH_SIDEBAR]
#define OC_COL_BASE        oc_theme[TH_BASE]
#define OC_COL_HEADER      oc_theme[TH_HEADER]
#define OC_COL_INPUT       oc_theme[TH_INPUT]
#define OC_COL_SELECT      oc_theme[TH_SELECT]
#define OC_COL_HOVER       oc_theme[TH_HOVER]
#define OC_COL_BORDER      oc_theme[TH_BORDER]
#define OC_COL_RAIL_ICON   oc_theme[TH_RAIL_ICON]
#define OC_COL_TEXT        oc_theme[TH_TEXT]
#define OC_COL_MUTED       oc_theme[TH_MUTED]
#define OC_COL_FAINT       oc_theme[TH_FAINT]
#define OC_COL_DANGER      oc_theme[TH_DANGER]
/* Deliberately NOT the accent: blue is the "primary / selected" colour
 * everywhere else, so a transient notice wearing it reads as a control rather
 * than a message. */
#define OC_COL_NOTICE      oc_theme[TH_NOTICE]
#define OC_COL_ONLINE      oc_theme[TH_ONLINE]
#define OC_COL_AWAY        oc_theme[TH_AWAY]
/* The ink of a label or icon drawn on an ACCENT, ACCENT_DIM or DANGER fill:
 * primary and selected buttons and chips, unread badges, the palette's
 * selected row. White in the dark and light palettes; part of the palette
 * rather than a literal because a high-contrast palette cannot carry white on
 * an accent that also reads on a near-black surface (REQ-262). */
#define OC_COL_ON_ACCENT   oc_theme[TH_ON_ACCENT]

/* --- the presence marker's shape (REQ-122) ---------------------------------
 *
 * Presence is never told by colour alone. Online and away were a green and an
 * amber dot, the pair red-green colour blindness confuses, so each state has a
 * SHAPE of its own and the colour only repeats it: online a filled disc, away a
 * half-filled disc (the left half, inside a ring), offline a ring, and
 * do-not-disturb a crescent. Do-not-disturb is a second axis, not a presence
 * value (REQ-122), but it replaces the shape whatever the presence under it,
 * as it always has.
 *
 * Decided here, once, so every place a dot is drawn shows the same shape and
 * the test can assert the four are distinct without rendering anything. */
enum { OC_PMARK_ONLINE = 0, OC_PMARK_AWAY, OC_PMARK_OFFLINE, OC_PMARK_DND,
       OC_PMARK_COUNT };

/* `presence` is the wire value (OC_PRESENCE_* in shared/protocol.h); anything
 * it does not name is offline. */
int oc_presence_mark(uint8_t presence, int dnd);

/* --- ink against the surface under it (REQ-262) ----------------------------
 *
 * WCAG asks 3:1 of non-text UI and more of text. Below 3:1 an element is not
 * dim, it is absent, so 3:1 is the floor a colour has to clear to be used at
 * all rather than a grade to aim past.
 *
 * `OC_INK_ON(ink, surface)` is how a faint ink is asked for. It answers with
 * that ink where the pair clears the floor and with the next-stronger one where
 * it does not, so a call site names the effect it wants and cannot quietly get
 * an unreadable one. Use it wherever FAINT or MUTED lands on a surface that is
 * not the plain canvas — a row fill, a field fill, a selection — and pass the
 * surface actually beneath, not the one usually beneath.
 *
 * The pairs are resolved when the theme is applied, so this costs a lookup.
 * TH_SELECT is derived from the colour scheme and has eight values, none of
 * them a constant; resolving after the derivation covers all of them without a
 * table naming any. tests/test_theme.c asserts the floor holds for every
 * (ink, surface, mode, scheme), which is what makes this a guarantee rather
 * than an intention. */
#define OC_CONTRAST_FLOOR 3.0f

uint32_t oc_ink_on(int ink, int surface);
#define OC_INK_ON(ink, surface) oc_ink_on((ink), (surface))

/* Exposed for the test, and for anyone checking a pair by hand. */
float oc_theme_luminance(uint32_t rgb);
float oc_theme_contrast(uint32_t a, uint32_t b);

#endif /* OC_GUI_THEME_H */
