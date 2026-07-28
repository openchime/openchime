/*
 * OpenChime Win32 GUI — theme palette (ARCH-82, REQ-262).
 *
 * The palette is a runtime array rather than a set of literals, so a theme can
 * be switched while the app runs (WIN-26). Call sites are unchanged: each
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
enum { OC_THEME_DARK = 0, OC_THEME_LIGHT = 1, OC_THEME_SYSTEM = 2 };

extern uint32_t oc_theme[TH_COUNT];

/* Apply a mode. OC_THEME_SYSTEM follows the Windows apps-use-light-theme
 * setting, resolving to dark when it cannot be read. */
void oc_theme_apply(int mode);
int  oc_theme_mode(void);            /* the mode last applied (not the resolved one) */
const char *oc_theme_mode_name(int mode);

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
