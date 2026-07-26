/*
 * OpenChime Win32 GUI — theme palette (ARCH-82). A deep-blue accent on a dark
 * neutral shell, tracking the TUI's information architecture with a graphical
 * look. Colors are 0xRRGGBB; oc_d2d_color() (d2d.h) turns them into
 * D2D1_COLOR_F. A light variant is deferred to a later phase.
 */
#ifndef OC_GUI_THEME_H
#define OC_GUI_THEME_H

/* Accent — deep blue (selection, unread dot, links, focus, send button). */
#define OC_COL_ACCENT      0x2563EB
#define OC_COL_ACCENT_DIM  0x1E40AF

/* Shell neutrals (darkest rail -> base transcript). */
#define OC_COL_RAIL        0x15181C
#define OC_COL_SIDEBAR     0x1A1D21
#define OC_COL_BASE        0x1E2227   /* transcript / main pane */
#define OC_COL_HEADER      0x1A1D21
#define OC_COL_SELECT      0x2A3138   /* selected row fill */
#define OC_COL_HOVER       0x262D35   /* row hover — perceptible but below SELECT */
#define OC_COL_BORDER      0x2C3238

/* Text. */
#define OC_COL_TEXT        0xE8EAED
#define OC_COL_MUTED       0x9AA0A6
#define OC_COL_FAINT       0x6B7178

/* Semantic. */
#define OC_COL_DANGER      0xE05252
#define OC_COL_ONLINE      0x3BA55D
#define OC_COL_AWAY        0xD9A441

#endif /* OC_GUI_THEME_H */
