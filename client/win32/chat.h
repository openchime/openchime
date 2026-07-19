/*
 * OpenChime Win32 GUI — the chat surface (ARCH-80). After the login window
 * authenticates, main.c hands the live oc_client here and this module builds the
 * paneled chat shell (channels · messages · members · composer) as native
 * comctl32 controls, rendering the shared view-model each tick and driving every
 * feature through the same slash-command grammar the TUI uses. Pure view + input;
 * all logic stays in client/core.
 */

#ifndef OC_WIN32_CHAT_H
#define OC_WIN32_CHAT_H

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "client.h"

/* Build the chat controls inside `hwnd` (login controls are hidden by the
 * caller) and adopt the authenticated client. Called once, on AUTH_OK. */
void chat_build(HWND hwnd, HINSTANCE inst, oc_client *cl);

/* Re-lay the panels to the current client area. Call from WM_SIZE. */
void chat_layout(HWND hwnd);

/* Fold the model into the controls. Call each WM_TIMER tick, after
 * oc_client_tick. Cheap when nothing changed (diffed against a cache). */
void chat_render(void);

/* Route a WM_COMMAND from a chat control (Send button, list selection). Returns
 * non-zero if it was a chat control and handled. */
int chat_command(HWND hwnd, WPARAM wp, LPARAM lp);

/* True once chat_build has run (main.c uses this to decide routing). */
int chat_active(void);

#endif /* OC_WIN32_CHAT_H */
