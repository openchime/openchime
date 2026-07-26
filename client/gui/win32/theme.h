/*
 * OpenChime Win32 GUI — theme palette (ARCH-82). A deep-blue accent on a dark
 * neutral shell, tracking the TUI's information architecture with a graphical
 * look. Colors are 0xRRGGBB; oc_d2d_color() (d2d.h) turns them into
 * D2D1_COLOR_F. A light variant is deferred to a later phase.
 */
#ifndef OC_GUI_THEME_H
#define OC_GUI_THEME_H

/* Accent — deep blue (selection, unread dot, links, focus, send button). */
#define OC_COL_ACCENT      0x3B82F6   /* brighter blue — pops on the dark shell */
#define OC_COL_ACCENT_DIM  0x2563EB

/* Shell neutrals, with real elevation steps (darkest rail -> lightest canvas).
 * A wider spread than before so the panes read as distinct layers. */
#define OC_COL_RAIL        0x121519   /* left rail — deepest */
#define OC_COL_SIDEBAR     0x181B21   /* channel sidebar + header */
#define OC_COL_BASE        0x1F242B   /* transcript / main canvas — lightest */
#define OC_COL_HEADER      0x181B21
#define OC_COL_INPUT       0x262B33   /* composer field — a raised surface */
#define OC_COL_SELECT      0x2B4067   /* selected row — blue-tinted toward accent */
#define OC_COL_HOVER       0x272C34   /* row hover — perceptible, below SELECT */
#define OC_COL_BORDER      0x2E333B

/* Text. */
#define OC_COL_TEXT        0xEBEDF0
#define OC_COL_MUTED       0x9BA2AD
#define OC_COL_FAINT       0x6A7079

/* Semantic. */
#define OC_COL_DANGER      0xE05252
#define OC_COL_ONLINE      0x3BA55D
#define OC_COL_AWAY        0xD9A441

#endif /* OC_GUI_THEME_H */
