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

/* Apply a mode. OC_THEME_SYSTEM follows the Windows apps-use-light-theme
 * setting, resolving to dark when it cannot be read. */
void oc_theme_apply(int mode);
/* Is the palette in force the LIGHT one? Not the same question as the mode: SYSTEM
 * resolves to either. Anything outside theme.c that has to match the shell — the
 * window caption, for one — asks this rather than re-reading the registry. */
int  oc_theme_is_light(void);
int  oc_theme_mode(void);            /* the mode last applied (not the resolved one) */
const char *oc_theme_mode_name(int mode);

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
 * OC_R_AVATAR_* are the rounded squares standing in for a person or a
 * workspace. They stay proportional to the tile rather than fixed: a 36px tile
 * and an 18px one with the same radius do not read as the same shape. */
#define OC_R_OVERLAY     8.0f
#define OC_R_CONTROL     4.0f
#define OC_R_PILL      999.0f
#define OC_R_AVATAR_LG  12.0f   /* the 36px tile: rail, menu header, sign-in mark */
#define OC_R_AVATAR_SM   5.0f   /* 18-20px, in a row or a chip */

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

#endif /* OC_GUI_THEME_H */
