/*
 * OpenChime Win32 GUI — theme palette (ARCH-82). A deep-blue accent on a dark
 * neutral shell, tracking the TUI's information architecture with a graphical
 * look. Colors are 0xRRGGBB; oc_d2d_color() (d2d.h) turns them into
 * D2D1_COLOR_F. A light variant is deferred to a later phase.
 */
#ifndef OC_GUI_THEME_H
#define OC_GUI_THEME_H

/* Accent — deep blue (selection, unread dot, links, focus, send button). */
#define OC_COL_ACCENT      0x3D8BFF   /* brighter blue — pops on the dark shell */
#define OC_COL_ACCENT_DIM  0x2563EB

/* Shell neutrals, with real elevation steps (darkest rail -> lightest canvas).
 * A wider spread than before so the panes read as distinct layers. */
#define OC_COL_RAIL        0x0B1220   /* left rail — deepest */
#define OC_COL_SIDEBAR     0x111C33   /* channel sidebar + header */
#define OC_COL_BASE        0x162238   /* transcript / main canvas — lightest */
#define OC_COL_HEADER      0x111C33
#define OC_COL_INPUT       0x1E2E4C   /* composer field — a raised surface */
#define OC_COL_SELECT      0x2C4E86   /* selected row — blue-tinted toward accent */
#define OC_COL_HOVER       0x1D2C48   /* row hover — perceptible, below SELECT */
#define OC_COL_BORDER      0x27395C

/* Text. */
#define OC_COL_TEXT        0xE9EDF5
#define OC_COL_MUTED       0x93A1BC
#define OC_COL_FAINT       0x63708C

/* Semantic. */
#define OC_COL_DANGER      0xE05252
#define OC_COL_ONLINE      0x3BA55D
#define OC_COL_AWAY        0xD9A441

#endif /* OC_GUI_THEME_H */
