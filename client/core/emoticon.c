/* Typed emoticons turned into emoji (emoticon.h). */
#include "emoticon.h"

#include "richtext.h"

#include <string.h>

/* Small and unambiguous: a few that read as faces in any text and nothing else.
 * Longest first where one begins another, so `:-)` is not taken as `:-` and `)`. */
static const struct { const char *text, *emoji; } TABLE[] = {
    { "</3", "\xF0\x9F\x92\x94" },   /* 💔 */
    { ":'(", "\xF0\x9F\x98\xA2" },   /* 😢 */
    { ":-)", "\xF0\x9F\x99\x82" },   /* 🙂 */
    { ":-(", "\xF0\x9F\x99\x81" },   /* 🙁 */
    { ";-)", "\xF0\x9F\x98\x89" },   /* 😉 */
    { ":)",  "\xF0\x9F\x99\x82" },   /* 🙂 */
    { ":(",  "\xF0\x9F\x99\x81" },   /* 🙁 */
    { ":D",  "\xF0\x9F\x98\x84" },   /* 😄 */
    { ";)",  "\xF0\x9F\x98\x89" },   /* 😉 */
    { ":P",  "\xF0\x9F\x98\x9B" },   /* 😛 */
    { ":p",  "\xF0\x9F\x98\x9B" },   /* 😛 */
    { ":O",  "\xF0\x9F\x98\xAE" },   /* 😮 */
    { ":o",  "\xF0\x9F\x98\xAE" },   /* 😮 */
    { ":/",  "\xF0\x9F\x98\x95" },   /* 😕 */
    { ":|",  "\xF0\x9F\x98\x90" },   /* 😐 */
    { "<3",  "\xE2\x9D\xA4\xEF\xB8\x8F" },   /* ❤️ */
    { "B)",  "\xF0\x9F\x98\x8E" },   /* 😎 */
};

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Whether byte `at` lies in code: inside a span the renderer would draw as
 * code, or past a fence or a backtick not yet closed -- the composer holds text
 * still being typed, where an open code span has no closing delimiter yet. */
static int in_code(const char *text, size_t len, size_t at) {
    oc_rt_span sp[OC_RT_MAX];
    size_t n = oc_rt_scan(text, len, sp, OC_RT_MAX);
    if (n > OC_RT_MAX) n = OC_RT_MAX;
    for (size_t i = 0; i < n; i++)
        if ((sp[i].style & (OC_RT_CODE | OC_RT_CODEBLOCK)) && at >= sp[i].start && at < sp[i].start + sp[i].len)
            return 1;
    int fenced = 0, ticks = 0;
    for (size_t i = 0; i < at && i < len; i++) {
        if (text[i] == '`' && i + 2 < len && text[i + 1] == '`' && text[i + 2] == '`') {
            fenced = !fenced; ticks = 0; i += 2;
        } else if (text[i] == '\n') {
            ticks = 0;   /* inline code closes on its line */
        } else if (text[i] == '`' && !fenced) {
            ticks = !ticks;
        }
    }
    return fenced || ticks;
}

const char *oc_emoticon_before(const char *text, size_t len, size_t caret, size_t *start) {
    if (caret > len) return NULL;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) {
        size_t n = strlen(TABLE[i].text);
        if (caret < n) continue;
        size_t s = caret - n;
        if (memcmp(text + s, TABLE[i].text, n) != 0) continue;
        if (s > 0 && !is_space(text[s - 1])) continue;   /* part of a longer word */
        if (in_code(text, len, s)) return NULL;
        if (start) *start = s;
        return TABLE[i].emoji;
    }
    return NULL;
}

int oc_emoticon_convert(const char *in, size_t len, char *out, size_t cap) {
    size_t o = 0;
    int count = 0;
    for (size_t i = 0; i < len; ) {
        /* An emoticon ends where whitespace or the end follows. */
        size_t e = i;
        while (e < len && !is_space(in[e])) e++;
        size_t s = 0;
        const char *emoji = e > i ? oc_emoticon_before(in, len, e, &s) : NULL;
        if (emoji && s == i) {
            size_t el = strlen(emoji);
            if (o + el >= cap) goto full;
            memcpy(out + o, emoji, el); o += el;
            count++;
        } else {
            if (o + (e - i) >= cap) goto full;
            memcpy(out + o, in + i, e - i); o += e - i;
        }
        i = e;
        while (i < len && is_space(in[i])) {
            if (o + 1 >= cap) goto full;
            out[o++] = in[i++];
        }
    }
    if (o >= cap) goto full;
    out[o] = '\0';
    return count;
full:
    if (cap) { size_t k = len < cap - 1 ? len : cap - 1; memcpy(out, in, k); out[k] = '\0'; }
    return -1;
}

int oc_emoticon_setting_on(const char *value) {
    return value && strcmp(value, "on") == 0;
}

const char *oc_emoticon_typed(int on, const char *text, size_t len, size_t caret, size_t *start) {
    return on ? oc_emoticon_before(text, len, caret, start) : NULL;
}

int oc_emoticon_send(int on, const char *in, size_t len, char *out, size_t cap) {
    if (on) return oc_emoticon_convert(in, len, out, cap);
    if (len >= cap) return -1;
    memcpy(out, in, len);
    out[len] = '\0';
    return 0;
}
