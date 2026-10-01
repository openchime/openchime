/*
 * OpenChime — in-app feedback (feedback.h).
 */

#include "feedback.h"

#include <stdio.h>
#include <string.h>

#define HOLD_MIN_MS 2000u           /* left after a hold ends, to finish reading */

void oc_fb_init(oc_fb *f, void (*say)(const char *, int)) {
    memset(f, 0, sizeof *f);
    f->next_id = 1;
    f->say = say;
}

static void speak(oc_fb *f, const char *text, int assertive) {
    if (f->say && text && text[0]) f->say(text, assertive);
}

uint64_t oc_fb_duration_ms(int kind, const char *text) {
    if (kind == OC_FB_UNDO) return 10000;
    if (kind != OC_FB_CONFIRM) return 0;
    size_t n = text ? strlen(text) : 0;
    uint64_t ms = 4000 + (n > 40 ? (uint64_t)(n - 40) * 60 : 0);
    return ms > 10000 ? 10000 : ms;
}

static void drop(oc_fb *f, int i) {
    for (int k = i; k < f->n - 1; k++) f->t[k] = f->t[k + 1];
    f->n--;
}

static void set_time(oc_fb_toast *t, uint64_t now) {
    uint64_t d = oc_fb_duration_ms(t->kind, t->text);
    t->deadline_ms = d ? now + d : 0;
    t->held = 0;
    t->held_left_ms = 0;
}

uint32_t oc_fb_show(oc_fb *f, int kind, const char *text, const char *action, int action_id, uint64_t now) {
    if (!text || !text[0]) return 0;
    for (int i = 0; i < f->n; i++)
        if (f->t[i].kind == kind && strcmp(f->t[i].text, text) == 0) {
            set_time(&f->t[i], now);
            speak(f, text, kind == OC_FB_FAILED);
            return f->t[i].id;
        }
    if (f->n == OC_FB_TOASTS) {
        int victim = -1;
        for (int i = 0; i < f->n && victim < 0; i++) if (f->t[i].kind == OC_FB_CONFIRM) victim = i;
        for (int i = 0; i < f->n && victim < 0; i++) if (f->t[i].kind != OC_FB_PROGRESS) victim = i;
        if (victim < 0) victim = 0;
        drop(f, victim);
    }
    oc_fb_toast *t = &f->t[f->n++];
    memset(t, 0, sizeof *t);
    t->id = f->next_id++;
    if (!f->next_id) f->next_id = 1;
    t->kind = kind;
    snprintf(t->text, sizeof t->text, "%s", text);
    snprintf(t->action, sizeof t->action, "%s", action ? action : "");
    t->action_id = action_id;
    set_time(t, now);
    speak(f, text, kind == OC_FB_FAILED);
    return t->id;
}

static int index_of(const oc_fb *f, uint32_t id) {
    for (int i = 0; id && i < f->n; i++) if (f->t[i].id == id) return i;
    return -1;
}

const oc_fb_toast *oc_fb_find(const oc_fb *f, uint32_t id) {
    int i = index_of(f, id);
    return i < 0 ? NULL : &f->t[i];
}

int oc_fb_update(oc_fb *f, uint32_t id, int kind, const char *text, uint64_t now) {
    int i = index_of(f, id);
    if (i < 0 || !text || !text[0]) return 0;
    oc_fb_toast *t = &f->t[i];
    int changed = t->kind != kind || strcmp(t->text, text) != 0;
    t->kind = kind;
    snprintf(t->text, sizeof t->text, "%s", text);
    if (kind != OC_FB_UNDO) { t->action[0] = '\0'; t->action_id = 0; }
    set_time(t, now);
    if (changed) speak(f, text, kind == OC_FB_FAILED);
    return 1;
}

void oc_fb_dismiss(oc_fb *f, uint32_t id) {
    int i = index_of(f, id);
    if (i >= 0) drop(f, i);
}

void oc_fb_hold(oc_fb *f, uint32_t id, uint64_t now) {
    for (int i = 0; i < f->n; i++) {
        oc_fb_toast *t = &f->t[i];
        if (t->id == id && !t->held) {
            t->held = 1;
            t->held_left_ms = t->deadline_ms ? (t->deadline_ms > now ? t->deadline_ms - now : 0) : 0;
        } else if (t->id != id && t->held) {
            t->held = 0;
            if (t->deadline_ms)
                t->deadline_ms = now + (t->held_left_ms > HOLD_MIN_MS ? t->held_left_ms : HOLD_MIN_MS);
        }
    }
}

int oc_fb_tick(oc_fb *f, uint64_t now) {
    int changed = 0;
    for (int i = f->n - 1; i >= 0; i--) {
        const oc_fb_toast *t = &f->t[i];
        if (t->held || !t->deadline_ms || now < t->deadline_ms) continue;
        drop(f, i);
        changed = 1;
    }
    return changed;
}

static int banner_index(const oc_fb *f, int id) {
    for (int i = 0; i < f->nb; i++) if (f->b[i].id == id) return i;
    return -1;
}

void oc_fb_banner_set(oc_fb *f, int id, int severity, const char *text, const char *action, int action_id) {
    if (!text || !text[0]) { oc_fb_banner_clear(f, id); return; }
    int i = banner_index(f, id);
    if (i < 0) {
        if (f->nb == OC_FB_BANNERS) return;
        i = f->nb++;
        memset(&f->b[i], 0, sizeof f->b[i]);
        f->b[i].id = id;
    }
    oc_fb_banner *b = &f->b[i];
    /* Said when it starts or changes what it says, not every time the same
     * state is set again (a countdown would otherwise speak each second). */
    int is_new = b->seq == 0 || b->severity != severity;
    b->severity = severity;
    snprintf(b->text, sizeof b->text, "%s", text);
    snprintf(b->action, sizeof b->action, "%s", action ? action : "");
    b->action_id = action_id;
    if (is_new) {
        b->seq = ++f->banner_seq;
        speak(f, text, severity == OC_FB_ERROR);
    }
}

void oc_fb_banner_clear(oc_fb *f, int id) {
    int i = banner_index(f, id);
    if (i < 0) return;
    for (int k = i; k < f->nb - 1; k++) f->b[k] = f->b[k + 1];
    f->nb--;
}

const oc_fb_banner *oc_fb_banner_top(const oc_fb *f) {
    const oc_fb_banner *top = NULL;
    for (int i = 0; i < f->nb; i++)
        if (!top || f->b[i].severity > top->severity ||
            (f->b[i].severity == top->severity && f->b[i].seq > top->seq))
            top = &f->b[i];
    return top;
}
