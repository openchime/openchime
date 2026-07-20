/*
 * tuikit — shared widget types. Every widget uses these.
 */

#ifndef TK_COMMON_H
#define TK_COMMON_H

typedef struct { int x, y, w, h; } tk_rect;

/* What a widget's handle() reports back to the app's event loop. */
typedef enum {
    TK_NONE = 0,   /* event ignored / not consumed */
    TK_USED,       /* event consumed, no higher-level action */
    TK_SELECT,     /* the user chose/submitted (Enter) */
    TK_CANCEL,     /* the user backed out (Esc) */
    TK_CHANGED     /* content changed (e.g. text edited, selection moved) */
} tk_result;

#endif /* TK_COMMON_H */
