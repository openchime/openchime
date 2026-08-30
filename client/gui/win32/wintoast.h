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
 * `sound` names an OS notification sound — a `ms-winsoundevent:` value —
 * which WINDOWS plays, not us (REQ-138). NULL means the system default; ""
 * means silence. Naming the OS's own sound rather than shipping a file is the
 * point: nothing to bundle, and it follows whatever the user has set for
 * notifications system-wide instead of talking over it.
 *
 * Returns 1 if the toast was handed to Windows, 0 if the caller must fall back. */
int oc_wintoast_show(const char *title, const char *body,
                     const char *tag, const char *group,
                     const char *arg, const char *sound);

/* Raise a toast that can be ACTED ON: a text box and up to `n_btn` buttons.
 *
 * This is where protocol activation runs out. Windows delivers an <input>'s
 * text only to a COM activator -- `activationType="protocol"` can carry buttons
 * (each is just a different URL) but never the typed reply. So a toast with a
 * reply box is activated through `oc_wintoast_activator_*` below, and one
 * without could have stayed on the simpler path but does not, because two
 * activation routes for one surface is two things to keep in step.
 *
 * `btn_arg[i]` comes back verbatim as the activation argument; the reply box's
 * text arrives beside it. */
int oc_wintoast_show_actions(const char *title, const char *body,
                             const char *tag, const char *group,
                             const char *arg, const char *sound,
                             const char *reply_placeholder,
                             const char *const *btn_label,
                             const char *const *btn_arg, int n_btn);

/* The activator. `cb` is called on the COM thread with the action's argument
 * and the reply box's text ("" when there was none); it must not block.
 *
 * `oc_wintoast_activator_register` is called by the ordinary client process so
 * a click can reach it. `oc_wintoast_activator_serve` is the -Embedding path:
 * Windows starts a second copy to service the activation, and that copy has no
 * window and no session -- it exists only to pass the click on. */
typedef void (*oc_wintoast_action_cb)(const char *arg, const char *reply);
int  oc_wintoast_activator_register(oc_wintoast_action_cb cb);
void oc_wintoast_activator_unregister(void);
/* The CLSID as a string, for the registry and the shortcut property. */
const char *oc_wintoast_activator_clsid(void);

/* Ensure a Start-menu shortcut carrying the AUMID and the activator CLSID.
 * Windows resolves BOTH through that shortcut for an unpackaged app, and Inno
 * cannot write the activator property -- so the app writes its own. Cheap and
 * idempotent: it rewrites only when missing or pointing elsewhere. */
int oc_wintoast_ensure_shortcut(const char *aumid, const char *display_name);

/* Release the factories. Called on shutdown; safe when init never succeeded. */
void oc_wintoast_done(void);

#endif /* OC_WINTOAST_H */
