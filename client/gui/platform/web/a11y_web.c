/* OpenChime web client -- the accessibility provider (REQ-269, ARCH-99) as
 * an ARIA mirror of the tree the application publishes.
 *
 * The page gets a hidden layer over the canvas: one element per published
 * item, positioned where the item is drawn, with the role and the name a
 * screen reader speaks -- the same two-level tree the UIA provider serves
 * (conversations, messages, the composer, buttons). The elements take no
 * pointer events, so the mouse still reaches the canvas; they take focus and
 * activation from assistive technology and the keyboard, and an activation
 * is handed to the application as an invoke event, the same token the UIA
 * route delivers, so both routes end in the call the mouse path makes.
 * Announcements go to a live region. The layer is rebuilt only when what the
 * application published changed. */
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a11y.h"
#include "platform.h"

static int g_ready;
static unsigned g_announced;
static uint64_t g_last_hash;

EMSCRIPTEN_KEEPALIVE void oc_web_a11y_invoke(uint32_t hi, uint32_t lo) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = oc_plat_event_type();
    e.user.code = OC_PLAT_EV_A11Y_INVOKE;
    e.user.data1 = (void *)(uintptr_t)hi;
    e.user.data2 = (void *)(uintptr_t)lo;
    SDL_PushEvent(&e);
}

void oc_a11y_init(SDL_Window *w) {
    (void)w;
    g_ready = EM_ASM_INT({
        if (!document.getElementById("canvas")) return 0;
        let layer = document.getElementById("oc-a11y");
        if (!layer) {
            layer = document.createElement("div");
            layer.id = "oc-a11y";
            layer.setAttribute("role", "application");
            layer.setAttribute("aria-label", "OpenChime");
            layer.style.cssText = "position:absolute;left:0;top:0;width:0;height:0;overflow:visible;pointer-events:none";
            document.body.appendChild(layer);
            const live = document.createElement("div");
            live.id = "oc-a11y-live";
            live.setAttribute("aria-live", "polite");
            live.style.cssText = "position:absolute;left:-10000px;width:1px;height:1px;overflow:hidden";
            document.body.appendChild(live);
            /* Enter or Space on a mirrored element activates it. */
            layer.addEventListener("keydown", (ev) => {
                if (ev.key !== "Enter" && ev.key !== " ") return;
                const t = ev.target; if (!t || !t.dataset.hi) return;
                ev.preventDefault();
                Module.ccall("oc_web_a11y_invoke", null, ["number", "number"], [parseInt(t.dataset.hi, 10), parseInt(t.dataset.lo, 10)]);
            });
            layer.addEventListener("click", (ev) => {
                const t = ev.target; if (!t || !t.dataset.hi) return;
                Module.ccall("oc_web_a11y_invoke", null, ["number", "number"], [parseInt(t.dataset.hi, 10), parseInt(t.dataset.lo, 10)]);
            });
        }
        return 1;
    });
}

void oc_a11y_shutdown(void) {
    EM_ASM({ const l = document.getElementById("oc-a11y"); if (l) l.innerHTML = ""; });
    g_ready = 0;
}

int oc_a11y_available(void) { return g_ready; }

static const char *role_of(oc_acc_kind k) {
    switch (k) {
    case OC_ACC_CONVERSATION: return "listitem";
    case OC_ACC_MESSAGE:      return "article";
    case OC_ACC_COMPOSER:     return "textbox";
    case OC_ACC_BUTTON:       return "button";
    case OC_ACC_TAB:          return "tab";
    case OC_ACC_LISTITEM:     return "listitem";
    }
    return "group";
}

/* JSON-escape into `out`; returns the length written. */
static size_t jesc(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

void oc_a11y_publish(const oc_acc_item *items, int n,
                     const uint16_t *composer, int caret, int anchor) {
    if (!g_ready) return;
    if (n < 0) n = 0;
    if (n > OC_ACC_MAX) n = OC_ACC_MAX;
    /* Only a changed tree reaches the page. */
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < n; i++) {
        const oc_acc_item *a = items + i;
        const unsigned char *p = (const unsigned char *)a;
        for (size_t k = 0; k < sizeof *a; k++) h = (h ^ p[k]) * 1099511628211ull;
    }
    h ^= (uint64_t)(uint32_t)caret << 32 ^ (uint32_t)anchor;
    for (const uint16_t *c = composer; c && *c; c++) h = (h ^ *c) * 1099511628211ull;
    if (h == g_last_hash) return;
    g_last_hash = h;

    size_t cap = (size_t)n * (OC_ACC_NAME_MAX * 2 + OC_ACC_AID_MAX + 128) + 64;
    char *json = malloc(cap);
    if (!json) return;
    size_t o = 0;
    o += (size_t)snprintf(json + o, cap - o, "[");
    for (int i = 0; i < n; i++) {
        const oc_acc_item *a = items + i;
        o += (size_t)snprintf(json + o, cap - o, "%s{\"r\":\"%s\",\"l\":%d,\"t\":%d,\"w\":%d,\"h\":%d,\"hi\":%u,\"lo\":%u,\"a\":\"",
                              i ? "," : "", role_of(a->kind), a->l, a->t, a->r - a->l, a->b - a->t,
                              (unsigned)(a->invoke >> 32), (unsigned)(a->invoke & 0xFFFFFFFFu));
        o += jesc(a->aid, json + o, cap - o);
        o += (size_t)snprintf(json + o, cap - o, "\",\"n\":\"");
        o += jesc(a->name, json + o, cap - o);
        o += (size_t)snprintf(json + o, cap - o, "\"}");
        if (o + 1024 > cap) break;
    }
    snprintf(json + o, cap - o, "]");
    EM_ASM({
        try {
            const items = JSON.parse(UTF8ToString($0));
            const layer = document.getElementById("oc-a11y");
            const canvas = document.getElementById("canvas");
            if (!layer || !canvas) return;
            const rc = canvas.getBoundingClientRect();
            const sx = rc.width / (canvas.width || 1);
            const sy = rc.height / (canvas.height || 1);
            const comp = UTF16ToString($1);
            let html = "";
            for (const it of items) {
                const style = "position:absolute;left:" + (rc.left + it.l * sx) + "px;top:" + (rc.top + it.t * sy) +
                              "px;width:" + (it.w * sx) + "px;height:" + (it.h * sy) + "px;pointer-events:none;opacity:0";
                const esc = (s) => s.split("&").join("&amp;").split('"').join("&quot;").split("<").join("&lt;");
                if (it.r === "textbox")
                    html += "<div role=\"textbox\" aria-multiline=\"true\" aria-label=\"" + esc(it.n) + "\" id=\"oc-a11y-composer\" tabindex=\"0\" style=\"" + style + "\">" + esc(comp) + "</div>";
                else
                    html += "<div role=\"" + it.r + "\" aria-label=\"" + esc(it.n) + "\" data-aid=\"" + esc(it.a) + "\"" +
                            (it.hi || it.lo ? " data-hi=\"" + it.hi + "\" data-lo=\"" + it.lo + "\" tabindex=\"0\"" : "") +
                            " style=\"" + style + "\"></div>";
            }
            layer.innerHTML = html;
        } catch (e) { console.error("a11y: " + e); }
    }, json, composer ? composer : (const uint16_t *)u"");
    free(json);
}

void oc_a11y_announce(const char *utf8) {
    if (!g_ready || !utf8) return;
    g_announced++;
    EM_ASM({ const l = document.getElementById("oc-a11y-live"); if (l) { l.setAttribute("aria-live", "polite"); l.textContent = UTF8ToString($0); } }, utf8);
}

void oc_a11y_announce_assertive(const char *utf8) {
    if (!g_ready || !utf8) return;
    g_announced++;
    EM_ASM({ const l = document.getElementById("oc-a11y-live"); if (l) { l.setAttribute("aria-live", "assertive"); l.textContent = UTF8ToString($0); } }, utf8);
}

unsigned oc_a11y_announced(void) { return g_announced; }

void oc_a11y_focus(oc_acc_kind kind, uint64_t id) {
    (void)kind; (void)id;   /* the mirrored element under the keyboard keeps its focus */
}
