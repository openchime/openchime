/* Real Windows notifications (REQ-138) — the Notification Center toast, not a
 * tray balloon.
 *
 * Its own translation unit because of what is inside it. The mingw headers ship
 * `roapi.h` and `winstring.h` but NOT `windows.ui.notifications.h` or
 * `windows.data.xml.dom.h`, so the interfaces are hand-declared: IIDs and vtable
 * structs written out by hand. That is ordinary for WinRT from C — WinRT is COM
 * underneath, so no C++/WinRT projection and no new toolchain is needed, and
 * ARCH-80's "pure C throughout" is intact — but a wrong vtable slot fails
 * silently or crashes, so it is kept in one place with a narrow API rather than
 * spread through the GUI.
 *
 * EVERY ENTRY POINT FAILS SOFT. There is no machine this is guaranteed to work
 * on: `combase.dll` may not resolve, the activation factory may not be
 * registered, and an unpackaged install with no AppUserModelID cannot raise a
 * toast at all. The caller falls back (balloon, then the client's own window),
 * which is the defect this whole area exists to remove — the old code dropped a
 * notification in silence when its one mechanism was unavailable.
 */

#ifndef OC_WINTOAST_H
#define OC_WINTOAST_H

#include <stddef.h>

/* Bind this process to `aumid` and resolve the WinRT entry points. Safe to call
 * more than once; the second call is a no-op. Returns 1 when a toast can
 * actually be raised, 0 when the caller must fall back.
 *
 * `aumid` must match the AppUserModelID on the Start-menu shortcut the
 * installer writes: Windows resolves a toast's identity through that shortcut,
 * and a mismatch is the failure that looks like nothing happening. */
int oc_wintoast_init(const char *aumid);

/* Did init succeed? Answers the same question after the fact, for the delivery
 * chain and for the test dump — so the harness can assert WHICH backend ran
 * rather than inferring it from a screenshot. */
int oc_wintoast_available(void);

/* Raise one toast. `tag` and `group` replace an earlier toast with the same
 * pair rather than stacking — one line per conversation, not one per message.
 * `arg` is handed back to the activator when the toast is clicked, and is how
 * the click knows which conversation to open.
 *
 * `silent` suppresses Windows' own notification sound, for when the client is
 * playing a per-event sound of its own (REQ-138) — otherwise one message makes
 * two noises.
 *
 * Returns 1 if the toast was handed to Windows, 0 if the caller must fall back. */
int oc_wintoast_show(const char *title, const char *body,
                     const char *tag, const char *group,
                     const char *arg, int silent);

/* Release the factories. Called on shutdown; safe when init never succeeded. */
void oc_wintoast_done(void);

#endif /* OC_WINTOAST_H */
