/*
 * OpenChime — OS notifications on Windows (osnotify.h): the Notification Center
 * toast (wintoast.c), then the tray balloon.
 *
 * The toast is the notification: it carries the reply box and buttons, replaces
 * per conversation (tag = key, group = workspace), is withdrawn from the
 * Notification Center when read, and names its sound for Windows to play, so
 * the sound obeys Focus Assist. It needs identity -- an AppUserModelID on a
 * Start-menu shortcut, and the activator registered for its buttons -- which
 * init sets up and proves.
 *
 * The balloon is what is left when the toast cannot be raised: the tray icon's
 * NIF_INFO, addressed by the window that owns the icon and its id. It needs no
 * identity, but carries no button, no replace and no sound, so it reports
 * OC_OSN_SHOWN_SILENT and the caller plays the sound.
 */

#include "osnotify.h"
#include "wintoast.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <string.h>

static int  g_toast_ok, g_forced_off;
static HWND g_tray_hwnd;
static UINT g_tray_id;

/* Windows' own notification sounds, by OC_OSN_SOUND_*: named in the toast for
 * Windows to play. "" is silence. */
static const char *const WINSOUND[OC_OSN_SOUND_COUNT] = {
    "ms-winsoundevent:Notification.Default", "ms-winsoundevent:Notification.IM",
    "ms-winsoundevent:Notification.Mail",    "ms-winsoundevent:Notification.Reminder", "",
};

int oc_osn_init(const char *app_id, const char *display_name, void *window, unsigned tray_id,
                oc_osn_action_cb cb) {
    g_tray_hwnd = (HWND)window;
    g_tray_id = tray_id;
    g_toast_ok = oc_wintoast_init(app_id);
    if (g_toast_ok) {
        oc_wintoast_ensure_shortcut(app_id, display_name ? display_name : app_id);
        if (cb) oc_wintoast_activator_register((oc_wintoast_action_cb)cb);
    }
    return g_toast_ok || g_tray_hwnd;
}

static int toast_ok(void) { return g_toast_ok && !g_forced_off; }

int oc_osn_available(void) { return toast_ok() || g_tray_hwnd != NULL; }

unsigned oc_osn_caps(void) {
    return toast_ok() ? OC_OSN_CAP_ACTIONS | OC_OSN_CAP_REPLY | OC_OSN_CAP_REPLACE | OC_OSN_CAP_WITHDRAW |
                        OC_OSN_CAP_SOUND
                      : 0;
}

/* The tray balloon: title and body only. */
static int balloon(const char *title, const char *body) {
    if (!g_tray_hwnd) return 0;
    NOTIFYICONDATAW nd;
    memset(&nd, 0, sizeof nd);
    nd.cbSize = sizeof nd;
    nd.hWnd = g_tray_hwnd;
    nd.uID = g_tray_id;
    nd.uFlags = NIF_INFO;
    nd.dwInfoFlags = NIIF_NONE;
    MultiByteToWideChar(CP_UTF8, 0, title ? title : "", -1, nd.szInfoTitle, 64);
    MultiByteToWideChar(CP_UTF8, 0, body ? body : "", -1, nd.szInfo, 256);
    nd.szInfoTitle[63] = nd.szInfo[255] = 0;
    return Shell_NotifyIconW(NIM_MODIFY, &nd) ? 1 : 0;
}

int oc_osn_show(const oc_osn *n) {
    if (!n) return OC_OSN_NOT_SHOWN;
    int snd = n->sound >= 0 && n->sound < OC_OSN_SOUND_COUNT ? n->sound : OC_OSN_SOUND_DEFAULT;
    if (toast_ok()) {
        int shown = 0;
        if (n->n_btn > 0 || n->reply_placeholder)
            shown = oc_wintoast_show_actions(n->title, n->body, n->source, n->key, n->group, n->open_url,
                                             WINSOUND[snd], n->reply_placeholder, n->btn_label, n->btn_arg,
                                             n->n_btn);
        if (!shown) shown = oc_wintoast_show(n->title, n->body, n->key, n->group, n->open_url, WINSOUND[snd]);
        if (shown) return OC_OSN_SHOWN;
    }
    return balloon(n->title, n->body) ? OC_OSN_SHOWN_SILENT : OC_OSN_NOT_SHOWN;
}

int oc_osn_withdraw(const char *key, const char *group) {
    return toast_ok() && key && group ? oc_wintoast_withdraw(key, group) : 0;
}
int oc_osn_withdraw_group(const char *group) { return toast_ok() && group ? oc_wintoast_withdraw_group(group) : 0; }

void oc_osn_test_unavailable(int on) { g_forced_off = on; }

void oc_osn_done(void) { oc_wintoast_done(); g_toast_ok = 0; }
