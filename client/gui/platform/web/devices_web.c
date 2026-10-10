/* The browser's device list (devices_web.h). */
#include "devices_web.h"

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <string.h>

/* The cache, on the page: Module.ocDev = { list, at, pending }. A refresh is
 * one enumerateDevices in flight at a time; devicechange refreshes too. */
EM_JS(void, dev_js_refresh, (void), {
    const reg = Module.ocDev || (Module.ocDev = { list: [], at: 0, pending: null, watching: false });
    if (!navigator.mediaDevices) return;
    if (!reg.watching) {
        reg.watching = true;
        try { navigator.mediaDevices.addEventListener("devicechange", () => { reg.at = 0; dev_js_refresh(); }); } catch (e) {}
    }
    if (reg.pending) return;
    reg.pending = navigator.mediaDevices.enumerateDevices()
        .then((all) => { reg.list = all.map((d) => ({ kind: d.kind, id: d.deviceId || "default", label: d.label || "" })); reg.at = Date.now(); })
        .catch(() => {})
        .then(() => { reg.pending = null; });
});

/* Wait for the one in flight (the page's thread only). */
EM_ASYNC_JS(void, dev_js_wait, (void), {
    const reg = Module.ocDev;
    if (reg && reg.pending) { try { await reg.pending; } catch (e) {} }
});

/* The copy, from whatever the cache holds. */
EM_JS(int, dev_js_copy, (const char *kind, char *out, int cap, int stride, int id_len, int name_off, int name_len, int def_off), {
    const reg = Module.ocDev;
    if (!reg) return 0;
    const want = UTF8ToString(kind);
    const noun = want === "videoinput" ? "Camera " : want === "audioinput" ? "Microphone " : "Speaker ";
    let n = 0;
    for (const d of reg.list) {
        if (d.kind !== want || n >= cap) continue;
        const rec = out + n * stride;
        stringToUTF8(d.id, rec, id_len);
        stringToUTF8(d.label || (noun + (n + 1)), rec + name_off, name_len);
        if (def_off >= 0) HEAP32[(rec + def_off) >> 2] = d.id === "default" || n === 0 ? 1 : 0;
        n++;
    }
    /* An output the browser does not enumerate (Firefox, Safari): the default. */
    if (n === 0 && want === "audiooutput" && cap > 0 && reg.at) {
        stringToUTF8("default", out, id_len); stringToUTF8("Speakers", out + name_off, name_len);
        if (def_off >= 0) HEAP32[(out + def_off) >> 2] = 1;
        n = 1;
    }
    return n;
});

EM_JS(int, dev_js_age_ms, (void), {
    const reg = Module.ocDev;
    return reg && reg.at ? Date.now() - reg.at : -1;
});

struct copy_args { const char *kind; char *out; int cap, stride, id_len, name_off, name_len, def_off, n; };
static void copy_main(void *arg) {
    struct copy_args *a = arg;
    if (dev_js_age_ms() < 0 || dev_js_age_ms() > 2000) dev_js_refresh();
    a->n = dev_js_copy(a->kind, a->out, a->cap, a->stride, a->id_len, a->name_off, a->name_len, a->def_off);
}
static void refresh_main(void *arg) { (void)arg; dev_js_refresh(); }

static void on_main(void (*fn)(void *), void *arg) {
    if (emscripten_is_main_runtime_thread()) { fn(arg); return; }
    emscripten_proxy_sync(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), fn, arg);
}

int oc_webdev_list(const char *kind, char *out, int cap, int stride, int id_len, int name_off, int name_len, int def_off) {
    if (cap <= 0) return 0;
    memset(out, 0, (size_t)cap * (size_t)stride);
    if (emscripten_is_main_runtime_thread() && dev_js_age_ms() < 0) {
        /* The first ask on the page's thread: the answer, not an empty cache. */
        dev_js_refresh();
        dev_js_wait();
    }
    struct copy_args a = { kind, out, cap, stride, id_len, name_off, name_len, def_off, 0 };
    on_main(copy_main, &a);
    return a.n;
}

void oc_webdev_refresh(void) { on_main(refresh_main, NULL); }
