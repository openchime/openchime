#include "webpages.h"

#include "qrcodegen.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int oc_html_escape(char *out, size_t cap, size_t *o, const char *in, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char *rep = NULL;
        switch (in[i]) {
        case '&':  rep = "&amp;";  break;
        case '<':  rep = "&lt;";   break;
        case '>':  rep = "&gt;";   break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;";  break;
        default: break;
        }
        size_t rl = rep ? strlen(rep) : 1;
        if (*o + rl >= cap) return -1;
        if (rep) memcpy(out + *o, rep, rl); else out[*o] = in[i];
        *o += rl;
    }
    out[*o] = '\0';
    return 0;
}

/* A growing page. Once anything fails to fit, it stays failed. */
typedef struct { char *b; size_t len, cap; int bad; } pg;

static void grow(pg *p, size_t need) {
    if (p->bad || p->len + need < p->cap) return;
    size_t nc = p->cap ? p->cap : 4096;
    while (nc <= p->len + need) nc *= 2;
    char *g = realloc(p->b, nc);
    if (!g) { p->bad = 1; return; }
    p->b = g; p->cap = nc;
}

static void raw(pg *p, const char *s) {
    size_t n = strlen(s);
    grow(p, n + 1);
    if (p->bad) return;
    memcpy(p->b + p->len, s, n + 1);
    p->len += n;
}

static void esc(pg *p, const char *s) {
    size_t n = s ? strlen(s) : 0;
    grow(p, n * 6 + 1);
    if (p->bad) return;
    if (oc_html_escape(p->b, p->cap, &p->len, s ? s : "", n) != 0) p->bad = 1;
}

/* A query value: percent-encoded (RFC 3986 unreserved kept), then escaped for
 * the attribute it sits in -- which, percent-encoded, changes nothing. */
static void qval(pg *p, const char *s) {
    static const char HEX[] = "0123456789ABCDEF";
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        int plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.' || c == '~';
        char t[4] = { (char)c, 0, 0, 0 };
        if (!plain) { t[0] = '%'; t[1] = HEX[c >> 4]; t[2] = HEX[c & 15]; }
        raw(p, t);
    }
}

static const char HEAD[] =
    "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<meta name=\"referrer\" content=\"no-referrer\">\n<title>";
static const char STYLE[] =
    "</title>\n<style>\n"
    ":root{color-scheme:light dark}\n"
    "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "font:16px/1.5 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;background:#f3f4f6;color:#1d1c1d}\n"
    "main{width:100%;max-width:24rem;margin:2rem;padding:2rem;background:#fff;border:1px solid #d5d8de;"
    "border-radius:12px}\n"
    "h1{font-size:1.35rem;margin:0 0 .25rem}\n"
    "p{margin:.5rem 0;color:#5b5f66}\n"
    "label{display:block;margin:1rem 0 .3rem;font-weight:600;font-size:.95rem}\n"
    "input{box-sizing:border-box;width:100%;padding:.6rem .7rem;font:inherit;border:1px solid #c4c8cf;"
    "border-radius:8px;background:inherit;color:inherit}\n"
    "button{margin-top:1.4rem;width:100%;padding:.7rem;font:inherit;font-weight:600;color:#fff;"
    "background:#1264a3;border:0;border-radius:8px;cursor:pointer}\n"
    ".msg{margin:1rem 0 0;padding:.6rem .75rem;border-radius:8px;background:#fdecea;color:#b3261e}\n"
    ".alt{margin-top:1.25rem;font-size:.92rem}\n"
    "a{color:#1264a3}\n"
    "pre,code{font:15px/1.6 ui-monospace,Consolas,monospace}\n"
    ".qr{display:block;margin:1rem auto}\n"
    "@media (prefers-color-scheme:dark){body{background:#1a1d21;color:#e8e9ea}"
    "main{background:#222529;border-color:#3a3d42}p{color:#a6a9ad}input{border-color:#4a4e54}"
    ".msg{background:#3b1f1d;color:#f2b8b5}a{color:#6cb3f0}}\n"
    "</style>\n</head>\n<body>\n<main>\n";
static const char TAIL[] = "</main>\n</body>\n</html>\n";

static void open_page(pg *p, const char *title) {
    raw(p, HEAD); esc(p, title); raw(p, STYLE);
}

static void message(pg *p, const char *m) {
    if (!m || !m[0]) return;
    raw(p, "<p class=\"msg\" role=\"alert\">"); esc(p, m); raw(p, "</p>\n");
}

static void hidden(pg *p, const char *name, const char *value) {
    raw(p, "<input type=\"hidden\" name=\""); raw(p, name); raw(p, "\" value=\""); esc(p, value); raw(p, "\">\n");
}

/* A field: `ac` its autocomplete purpose, which is what lets a password manager
 * fill a sign-in and save a new password. */
static void field(pg *p, const char *id, const char *label, const char *type, const char *ac,
                  const char *value, int focus) {
    raw(p, "<label for=\""); raw(p, id); raw(p, "\">"); esc(p, label); raw(p, "</label>\n");
    raw(p, "<input id=\""); raw(p, id); raw(p, "\" name=\""); raw(p, id); raw(p, "\" type=\""); raw(p, type);
    raw(p, "\" autocomplete=\""); raw(p, ac); raw(p, "\" required");
    if (value && value[0]) { raw(p, " value=\""); esc(p, value); raw(p, "\""); }
    if (focus) raw(p, " autofocus");
    raw(p, ">\n");
}

/* The query that carries a sign-in from one page to the next. */
static void carry(pg *p, const oc_page *pp) {
    raw(p, "redirect_uri="); qval(p, pp->redirect_uri);
    raw(p, "&amp;nonce="); qval(p, pp->nonce);
}

/* `text` as a QR code, inline SVG: the policy fetches nothing, so the image is
 * the page's own markup -- one path of unit squares, a quiet zone round it. */
static void qr_svg(pg *p, const char *text) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) return;
    int n = qrcodegen_getSize(qr);
    char b[192];
    snprintf(b, sizeof b, "<svg class=\"qr\" role=\"img\" aria-label=\"QR code\" viewBox=\"0 0 %d %d\" "
             "width=\"200\" height=\"200\" shape-rendering=\"crispEdges\">", n + 8, n + 8);
    raw(p, b);
    snprintf(b, sizeof b, "<rect width=\"%d\" height=\"%d\" fill=\"white\"/><path fill=\"black\" d=\"", n + 8, n + 8);
    raw(p, b);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y)) { snprintf(b, sizeof b, "M%d %dh1v1h-1z", x + 4, y + 4); raw(p, b); }
    raw(p, "\"/></svg>\n");
}

char *oc_page_render(const oc_page *pp, size_t *len) {
    pg p = { 0 };
    const char *user = pp->username ? pp->username : "";
    switch (pp->kind) {
    case OC_PAGE_SIGNIN:
        open_page(&p, "Sign in \xE2\x80\x94 OpenChime");
        raw(&p, "<h1>Sign in</h1>\n<p>to your OpenChime workspace</p>\n");
        message(&p, pp->message);
        raw(&p, "<form method=\"post\" action=\"signin\">\n");
        hidden(&p, "redirect_uri", pp->redirect_uri);
        hidden(&p, "nonce", pp->nonce);
        field(&p, "username", "Username", "text", "username", user, !user[0]);
        field(&p, "password", "Password", "password", "current-password", "", user[0] != 0);
        raw(&p, "<button type=\"submit\">Sign in</button>\n</form>\n");
        raw(&p, "<p class=\"alt\">Have an invitation? <a href=\"signup?");
        carry(&p, pp);
        raw(&p, "\">Create an account</a></p>\n");
        raw(&p, "<p class=\"alt\"><a href=\"account/password\">Change your password</a> \xC2\xB7 "
                "<a href=\"account/security\">Two-step sign-in</a></p>\n");
        break;
    case OC_PAGE_SIGNUP:
        open_page(&p, "Create an account \xE2\x80\x94 OpenChime");
        raw(&p, "<h1>Create an account</h1>\n<p>with the invitation you were given</p>\n");
        message(&p, pp->message);
        raw(&p, "<form method=\"post\" action=\"signup\">\n");
        hidden(&p, "redirect_uri", pp->redirect_uri);
        hidden(&p, "nonce", pp->nonce);
        field(&p, "invite", "Invitation", "text", "off", pp->invite, !(pp->invite && pp->invite[0]));
        field(&p, "username", "Username", "text", "username", user, pp->invite && pp->invite[0]);
        field(&p, "password", "Password", "password", "new-password", "", 0);
        field(&p, "confirm", "Password again", "password", "new-password", "", 0);
        raw(&p, "<button type=\"submit\">Create account</button>\n</form>\n");
        raw(&p, "<p class=\"alt\">Have an account? <a href=\"signin?");
        carry(&p, pp);
        raw(&p, "\">Sign in</a></p>\n");
        break;
    case OC_PAGE_DEVICE:
        open_page(&p, "Sign in a device \xE2\x80\x94 OpenChime");
        if (pp->done) {
            raw(&p, "<h1>Signed in</h1>\n<p>Go back to your terminal: it is signing in now. "
                    "You can close this tab.</p>\n");
            break;
        }
        if (pp->denied) {
            raw(&p, "<h1>Sign-in refused</h1>\n<p>The terminal was not signed in. "
                    "You can close this tab.</p>\n");
            break;
        }
        if (!pp->user_code || !pp->user_code[0]) {
            /* Step one: the code the terminal shows. */
            raw(&p, "<h1>Sign in a device</h1>\n<p>Enter the code your terminal shows.</p>\n");
            message(&p, pp->message);
            raw(&p, "<form method=\"get\" action=\"device\">\n");
            field(&p, "code", "Code", "text", "off", "", 1);
            raw(&p, "<button type=\"submit\">Continue</button>\n</form>\n");
            break;
        }
        /* Step two: who is asking, then the credentials to approve with. */
        raw(&p, "<h1>Sign in a device</h1>\n<p>A terminal is asking to sign in to this workspace with the code <b>");
        esc(&p, pp->user_code);
        raw(&p, "</b>, from <b>");
        esc(&p, pp->from && pp->from[0] ? pp->from : "an unknown address");
        {
            char ago[64];
            if (pp->minutes_ago == 0) snprintf(ago, sizeof ago, "</b>, just now.");
            else snprintf(ago, sizeof ago, "</b>, %u minute%s ago.", pp->minutes_ago, pp->minutes_ago == 1 ? "" : "s");
            raw(&p, ago);
        }
        raw(&p, " Only go on if that was you.</p>\n");
        message(&p, pp->message);
        raw(&p, "<form method=\"post\" action=\"device\">\n");
        hidden(&p, "code", pp->user_code);
        field(&p, "username", "Username", "text", "username", user, !user[0]);
        field(&p, "password", "Password", "password", "current-password", "", user[0] != 0);
        raw(&p, "<button type=\"submit\" name=\"action\" value=\"approve\">Sign in the terminal</button>\n");
        raw(&p, "<button type=\"submit\" name=\"action\" value=\"deny\" formnovalidate "
                "style=\"background:none;color:inherit;border:1px solid #c4c8cf;margin-top:.6rem\">"
                "That wasn't me</button>\n</form>\n");
        break;
    case OC_PAGE_STEP:
        open_page(&p, "Two-step sign-in \xE2\x80\x94 OpenChime");
        raw(&p, "<h1>Two-step sign-in</h1>\n<p>Enter the code your authenticator app shows, "
                "or one of your recovery codes.</p>\n");
        message(&p, pp->message);
        raw(&p, "<form method=\"post\" action=\"");
        esc(&p, pp->action && pp->action[0] ? pp->action : "signin/verify");
        raw(&p, "\">\n");
        hidden(&p, "ticket", pp->ticket);
        field(&p, "code", "Code", "text", "one-time-code", "", 1);
        raw(&p, "<button type=\"submit\">Continue</button>\n</form>\n");
        break;
    case OC_PAGE_SECURITY:
        open_page(&p, "Two-step sign-in \xE2\x80\x94 OpenChime");
        raw(&p, "<h1>Two-step sign-in</h1>\n");
        switch (pp->sec) {
        case OC_SEC_SIGNIN:
            raw(&p, "<p>After your password, signing in will also ask for a code from an authenticator "
                    "app on your phone.</p>\n");
            message(&p, pp->message);
            raw(&p, "<form method=\"post\" action=\"security\">\n");
            field(&p, "username", "Username", "text", "username", user, !user[0]);
            field(&p, "password", "Password", "password", "current-password", "", user[0] != 0);
            raw(&p, "<button type=\"submit\">Continue</button>\n</form>\n");
            break;
        case OC_SEC_SETUP:
        case OC_SEC_CONFIRM:
            if (pp->sec == OC_SEC_SETUP) {
                raw(&p, "<p>Scan this with your authenticator app, or type the key into it.</p>\n");
                qr_svg(&p, pp->otpauth ? pp->otpauth : "");
                raw(&p, "<p>Key: <code>"); esc(&p, pp->secret); raw(&p, "</code></p>\n");
            }
            raw(&p, "<p>Then enter the code it shows, to turn two-step sign-in on.</p>\n");
            message(&p, pp->message);
            raw(&p, "<form method=\"post\" action=\"security\">\n");
            hidden(&p, "ticket", pp->ticket);
            hidden(&p, "action", "confirm");
            field(&p, "code", "Code", "text", "one-time-code", "", 1);
            raw(&p, "<button type=\"submit\">Turn on</button>\n</form>\n");
            break;
        case OC_SEC_CODES:
            raw(&p, "<p>Two-step sign-in is on. Keep these recovery codes somewhere safe: each signs you in "
                    "once if you lose your phone. They are not shown again.</p>\n<pre>");
            esc(&p, pp->codes);
            raw(&p, "</pre>\n<p>You can close this tab.</p>\n");
            break;
        case OC_SEC_ON:
            raw(&p, "<p>Two-step sign-in is on. To turn it off, enter a code from your authenticator app or "
                    "a recovery code.</p>\n");
            message(&p, pp->message);
            raw(&p, "<form method=\"post\" action=\"security\">\n");
            hidden(&p, "ticket", pp->ticket);
            hidden(&p, "action", "remove");
            field(&p, "code", "Code", "text", "one-time-code", "", 1);
            raw(&p, "<button type=\"submit\">Turn off</button>\n</form>\n");
            break;
        case OC_SEC_OFF:
            raw(&p, "<p>Two-step sign-in is off. You can close this tab.</p>\n");
            break;
        }
        break;
    case OC_PAGE_PASSWORD:
        open_page(&p, "Change your password \xE2\x80\x94 OpenChime");
        if (pp->done) {
            raw(&p, "<h1>Password changed</h1>\n<p>Sign in with your new password from now on. "
                    "You can close this tab.</p>\n");
            break;
        }
        raw(&p, "<h1>Change your password</h1>\n");
        message(&p, pp->message);
        raw(&p, "<form method=\"post\" action=\"password\">\n");
        field(&p, "username", "Username", "text", "username", user, !user[0]);
        field(&p, "current", "Current password", "password", "current-password", "", user[0] != 0);
        field(&p, "password", "New password", "password", "new-password", "", 0);
        field(&p, "confirm", "New password again", "password", "new-password", "", 0);
        raw(&p, "<button type=\"submit\">Change password</button>\n</form>\n");
        break;
    }
    raw(&p, TAIL);
    if (p.bad) { free(p.b); return NULL; }
    *len = p.len;
    return p.b;
}

#define STATIC_PAGE(name, title, h1, body)                                              \
    static const char name[] = "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"           \
        "<meta charset=\"utf-8\">\n<meta name=\"viewport\" "                             \
        "content=\"width=device-width,initial-scale=1\">\n<title>" title "</title>\n"    \
        "<style>body{font:16px/1.5 system-ui,sans-serif;max-width:28rem;margin:4rem auto;" \
        "padding:0 1rem}</style>\n</head>\n<body>\n<h1>" h1 "</h1>\n<p>" body "</p>\n"   \
        "</body>\n</html>\n"

STATIC_PAGE(INVALID, "OpenChime", "This sign-in link isn&#39;t valid",
            "Go back to OpenChime and start signing in again.");
STATIC_PAGE(UNAVAILABLE, "OpenChime", "This workspace doesn&#39;t use passwords",
            "Go back to OpenChime and sign in the way it offers.");

const char *oc_page_invalid(size_t *len) { *len = sizeof INVALID - 1; return INVALID; }
const char *oc_page_unavailable(size_t *len) { *len = sizeof UNAVAILABLE - 1; return UNAVAILABLE; }

int oc_page_headers(const char *redirect_uri, char *out, size_t cap) {
    /* The callback's origin: scheme://host:port of a loopback redirect, which the
     * caller has checked. IPv6 loopback cannot be written in a policy's host
     * grammar, so there the policy does not name form targets at all -- the
     * page still posts only to itself, and runs nothing. */
    char cb[64] = "";
    if (redirect_uri && strncmp(redirect_uri, "http://127.0.0.1", 16) == 0) {
        const char *e = redirect_uri + 16;
        if (*e == ':') { e++; while (*e >= '0' && *e <= '9') e++; }
        if ((size_t)(e - redirect_uri) < sizeof cb) snprintf(cb, sizeof cb, "%.*s", (int)(e - redirect_uri), redirect_uri);
    } else if (redirect_uri && strncmp(redirect_uri, "http://localhost", 16) == 0) {
        const char *e = redirect_uri + 16;
        if (*e == ':') { e++; while (*e >= '0' && *e <= '9') e++; }
        if ((size_t)(e - redirect_uri) < sizeof cb) snprintf(cb, sizeof cb, "%.*s", (int)(e - redirect_uri), redirect_uri);
    }
    int v6 = redirect_uri && strncmp(redirect_uri, "http://[::1]", 12) == 0;
    int n = snprintf(out, cap,
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; "
        "frame-ancestors 'none'; base-uri 'none'%s%s%s\r\n"
        "X-Frame-Options: DENY\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Cache-Control: no-store\r\n"
        "Referrer-Policy: no-referrer\r\n",
        v6 ? "" : "; form-action 'self'", cb[0] ? " " : "", cb);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
