/*
 * OpenChime -- OS notifications in a browser (osnotify.h): the Notification
 * API. A notification carries a title, a body and a tag (the conversation),
 * so a second for the same conversation replaces the first and one can be
 * taken back; a click focuses the page and hands the notification's URL to
 * the application as a second launch would (oc_web_handoff). Buttons and a
 * reply box need a service worker, which the page does not have yet, so the
 * capabilities say so and the application keeps its own toasts for those.
 */
#include "osnotify.h"
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

static int g_forced_off, g_granted;

int oc_osn_init(const char *app_id, const char *display_name, void *window, unsigned tray_id,
                oc_osn_action_cb cb) {
    (void)app_id; (void)display_name; (void)window; (void)tray_id; (void)cb;
    g_granted = EM_ASM_INT({
        if (!("Notification" in window)) return 0;
        if (Notification.permission === "default") Notification.requestPermission().catch(() => {});
        return Notification.permission === "granted" ? 1 : 0;
    });
    return g_granted;
}

static int usable(void) {
    if (g_forced_off) return 0;
    g_granted = EM_ASM_INT({ return ("Notification" in window) && Notification.permission === "granted" ? 1 : 0; });
    return g_granted;
}

int      oc_osn_available(void) { return usable(); }
unsigned oc_osn_caps(void) { return usable() ? OC_OSN_CAP_REPLACE | OC_OSN_CAP_WITHDRAW : 0; }

int oc_osn_show(const oc_osn *n) {
    if (!usable() || !n) return OC_OSN_NOT_SHOWN;
    char tag[320];
    snprintf(tag, sizeof tag, "%s|%s", n->group ? n->group : "", n->key ? n->key : "");
    EM_ASM({
        try {
            const tag = UTF8ToString($2);
            const url = UTF8ToString($3);
            window.ocNotes = window.ocNotes || {};
            const o = new Notification(UTF8ToString($0), { body: UTF8ToString($1), tag: tag, silent: $4 ? true : false });
            o.onclick = () => {
                window.focus();
                if (url && Module.ccall) Module.ccall("oc_web_handoff", null, ["string", "string"], ["url", url]);
                o.close();
            };
            window.ocNotes[tag] = o;
        } catch (e) {}
    }, n->title ? n->title : "", n->body ? n->body : "", tag, n->open_url ? n->open_url : "",
       n->sound == OC_OSN_SOUND_SILENT);
    return n->sound == OC_OSN_SOUND_SILENT ? OC_OSN_SHOWN_SILENT : OC_OSN_SHOWN;
}

int oc_osn_withdraw(const char *key, const char *group) {
    char tag[320];
    snprintf(tag, sizeof tag, "%s|%s", group ? group : "", key ? key : "");
    return EM_ASM_INT({
        const tag = UTF8ToString($0);
        const o = window.ocNotes && window.ocNotes[tag];
        if (!o) return 0;
        o.close(); delete window.ocNotes[tag];
        return 1;
    }, tag);
}

int oc_osn_withdraw_group(const char *group) {
    return EM_ASM_INT({
        const g = UTF8ToString($0) + "|";
        let n = 0;
        for (const k of Object.keys(window.ocNotes || {}))
            if (k.startsWith(g)) { window.ocNotes[k].close(); delete window.ocNotes[k]; n++; }
        return n;
    }, group ? group : "");
}

void oc_osn_test_unavailable(int on) { g_forced_off = on; }
void oc_osn_done(void) { oc_osn_withdraw_group(""); }
