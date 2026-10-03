#include "webpages.h"

#include "qrcodegen.h"

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

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
    "<meta name=\"referrer\" content=\"same-origin\">\n<title>";
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
    ".acts{display:flex;gap:.6rem;margin-top:1rem}\n"
    ".acts a,.acts button{flex:1;box-sizing:border-box;margin:0;padding:.6rem;font-weight:600;text-align:center;"
    "text-decoration:none;color:inherit;background:none;border:1px solid #c4c8cf;border-radius:8px}\n"
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

/* The passkey script (AUTH.md §8.6): the one script any page runs, and only a
 * page on the workspace's trusted name that offers a passkey. It reads the
 * ceremony from the button's data attributes, asks the browser, and posts the
 * answer with the form it sits in; it fetches nothing and builds no markup. */
static const char WEBAUTHN_JS[] =
    "(function(){\n"
    "function b64u(b){var s='',a=new Uint8Array(b);for(var i=0;i<a.length;i++)s+=String.fromCharCode(a[i]);"
    "return btoa(s).replace(/\\+/g,'-').replace(/\\//g,'_').replace(/=+$/,'');}\n"
    "function unb(s){s=s.replace(/-/g,'+').replace(/_/g,'/');while(s.length%4)s+='=';"
    "var r=atob(s),a=new Uint8Array(r.length);for(var i=0;i<r.length;i++)a[i]=r.charCodeAt(i);return a;}\n"
    "function ids(s){return (s||'').split(',').filter(Boolean).map(function(c){return {type:'public-key',id:unb(c)};});}\n"
    "var el=document.getElementById('passkey');\n"
    "if(!el||!window.PublicKeyCredential)return;\n"
    "el.hidden=false;\n"
    "el.addEventListener('click',function(){\n"
    "var d=el.dataset,f=el.form;\n"
    "if(d.mode==='create'){navigator.credentials.create({publicKey:{challenge:unb(d.challenge),"
    "rp:{id:d.rp,name:'OpenChime'},user:{id:unb(d.user),name:d.name,displayName:d.name},"
    "pubKeyCredParams:[{type:'public-key',alg:-7},{type:'public-key',alg:-257}],"
    "authenticatorSelection:{userVerification:'preferred'},attestation:'none',excludeCredentials:ids(d.creds)}})"
    ".then(function(c){f.pk_cd.value=b64u(c.response.clientDataJSON);f.pk_att.value=b64u(c.response.attestationObject);f.submit();})"
    ".catch(function(){});}\n"
    "else{navigator.credentials.get({publicKey:{challenge:unb(d.challenge),rpId:d.rp,"
    "allowCredentials:ids(d.creds),userVerification:'preferred'}})"
    ".then(function(c){f.pk_cred.value=b64u(c.rawId);f.pk_cd.value=b64u(c.response.clientDataJSON);"
    "f.pk_ad.value=b64u(c.response.authenticatorData);f.pk_sig.value=b64u(c.response.signature);f.submit();})"
    ".catch(function(){});}\n"
    "});\n"
    "})();\n";

const char *oc_webauthn_js(size_t *len) { *len = sizeof WEBAUTHN_JS - 1; return WEBAUTHN_JS; }

/* A script's SRI value, "sha256-<base64>", into `sri`. */
static void integrity(const char *js, size_t n, char sri[64]) {
    uint8_t h[32]; size_t ol = 0;
    char b[48];
    if (mbedtls_sha256((const unsigned char *)js, n, h, 0) == 0 &&
        mbedtls_base64_encode((unsigned char *)b, sizeof b, &ol, h, sizeof h) == 0)
        snprintf(sri, 64, "sha256-%.*s", (int)ol, b);
}

const char *oc_webauthn_js_integrity(void) {
    static char sri[64];
    if (!sri[0]) integrity(WEBAUTHN_JS, sizeof WEBAUTHN_JS - 1, sri);
    return sri;
}

/* The recovery codes' copy button (AUTH.md §8.6): shown only where the
 * clipboard can take the codes, it copies the page's own list; it fetches
 * nothing and builds no markup. */
static const char CODES_JS[] =
    "(function(){\n"
    "var b=document.getElementById('copy'),c=document.getElementById('codes');\n"
    "if(!b||!c||!navigator.clipboard)return;\n"
    "b.hidden=false;\n"
    "b.addEventListener('click',function(){navigator.clipboard.writeText(c.textContent).then(function(){"
    "b.textContent='Copied';setTimeout(function(){b.textContent='Copy';},2000);},function(){});});\n"
    "})();\n";

const char *oc_codes_js(size_t *len) { *len = sizeof CODES_JS - 1; return CODES_JS; }

const char *oc_codes_js_integrity(void) {
    static char sri[64];
    if (!sri[0]) integrity(CODES_JS, sizeof CODES_JS - 1, sri);
    return sri;
}

/* The passkey button and the fields its answer fills, in the form it posts. */
static void passkey_button(pg *p, const oc_page *pp) {
    raw(p, "<input type=\"hidden\" name=\"pk_cd\" value=\"\">\n");
    if (pp->pk_mode == 1) {
        raw(p, "<input type=\"hidden\" name=\"pk_cred\" value=\"\">\n<input type=\"hidden\" name=\"pk_ad\" value=\"\">\n"
               "<input type=\"hidden\" name=\"pk_sig\" value=\"\">\n");
    } else {
        raw(p, "<input type=\"hidden\" name=\"pk_att\" value=\"\">\n");
    }
    raw(p, "<button type=\"button\" id=\"passkey\" hidden data-mode=\"");
    raw(p, pp->pk_mode == 2 ? "create" : "get");
    raw(p, "\" data-challenge=\""); esc(p, pp->pk_challenge);
    raw(p, "\" data-rp=\""); esc(p, pp->pk_rp);
    raw(p, "\" data-creds=\""); esc(p, pp->pk_creds);
    if (pp->pk_mode == 2) {
        raw(p, "\" data-user=\""); esc(p, pp->pk_user);
        raw(p, "\" data-name=\""); esc(p, pp->pk_name);
    }
    raw(p, "\" style=\"background:none;color:inherit;border:1px solid #c4c8cf;margin-top:.6rem\">");
    raw(p, pp->pk_mode == 2 ? "Add a passkey" : "Use a passkey");
    raw(p, "</button>\n");
}

static void passkey_script(pg *p) {
    raw(p, "<script src=\"webauthn.js\" integrity=\""); raw(p, oc_webauthn_js_integrity()); raw(p, "\"></script>\n");
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
        raw(&p, "<button type=\"submit\">Continue</button>\n");
        if (pp->pk_mode == 1) passkey_button(&p, pp);
        raw(&p, "</form>\n");
        if (pp->pk_mode == 1) passkey_script(&p);
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
                    "once if you lose your phone. They are not shown again.</p>\n<pre id=\"codes\">");
            esc(&p, pp->codes);
            raw(&p, "</pre>\n");
            /* Saved as a file the page itself holds, so nothing is fetched. */
            raw(&p, "<div class=\"acts\"><a href=\"data:text/plain;charset=utf-8,");
            qval(&p, "OpenChime recovery codes");
            if (user[0]) { qval(&p, " for "); qval(&p, user); }
            qval(&p, ". Each signs you in once.\n\n");
            qval(&p, pp->codes);
            raw(&p, "\" download=\"openchime-recovery-codes.txt\">Download</a>"
                    "<button type=\"button\" id=\"copy\" hidden>Copy</button></div>\n");
            raw(&p, "<script src=\"../codes.js\" integrity=\""); raw(&p, oc_codes_js_integrity());
            raw(&p, "\"></script>\n<p>You can close this tab.</p>\n");
            break;
        case OC_SEC_ON:
            raw(&p, "<p>Two-step sign-in is on. Enter a code from your authenticator app or a recovery code "
                    "to turn it off");
            raw(&p, pp->pk_rp && pp->pk_rp[0] ? ", or to add a passkey.</p>\n" : ".</p>\n");
            message(&p, pp->message);
            raw(&p, "<form method=\"post\" action=\"security\">\n");
            hidden(&p, "ticket", pp->ticket);
            field(&p, "code", "Code", "text", "one-time-code", "", 1);
            if (pp->pk_rp && pp->pk_rp[0])
                raw(&p, "<button type=\"submit\" name=\"action\" value=\"passkey\">Add a passkey</button>\n");
            raw(&p, "<button type=\"submit\" name=\"action\" value=\"remove\">Turn off</button>\n</form>\n");
            break;
        case OC_SEC_PASSKEY:
            raw(&p, "<p>Add a passkey for this workspace: your device asks you to confirm. A passkey works here, "
                    "at this address; after the workspace is renamed, add it again.</p>\n");
            message(&p, pp->message);
            raw(&p, "<form method=\"post\" action=\"security\">\n");
            hidden(&p, "ticket", pp->ticket);
            hidden(&p, "action", "passkey_add");
            passkey_button(&p, pp);
            raw(&p, "</form>\n");
            passkey_script(&p);
            break;
        case OC_SEC_PASSKEY_DONE:
            raw(&p, "<p>Passkey added. Signing in here can use it in place of a code. You can close this tab.</p>\n");
            break;
        case OC_SEC_OFF:
            raw(&p, "<p>Two-step sign-in is off. You can close this tab.</p>\n");
            break;
        }
        break;
    case OC_PAGE_RESET:
        open_page(&p, "Set a new password \xE2\x80\x94 OpenChime");
        if (pp->done) {
            raw(&p, "<h1>Password set</h1>\n<p>Sign in with your new password. You can close this tab.</p>\n");
            break;
        }
        raw(&p, "<h1>Set a new password</h1>\n<p>An administrator sent you this link. It works once, for a day.</p>\n");
        message(&p, pp->message);
        raw(&p, "<form method=\"post\" action=\"reset\">\n");
        hidden(&p, "t", pp->reset);
        field(&p, "password", "New password", "password", "new-password", "", 1);
        field(&p, "confirm", "New password again", "password", "new-password", "", 0);
        raw(&p, "<button type=\"submit\">Set password</button>\n</form>\n");
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
    return oc_page_headers_ex(redirect_uri, 0, out, cap);
}

int oc_page_headers_ex(const char *redirect_uri, int scripts, char *out, size_t cap) {
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
        "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; %s"
        "frame-ancestors 'none'; base-uri 'none'%s%s%s\r\n"
        "X-Frame-Options: DENY\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Cache-Control: no-store\r\n"
        "Referrer-Policy: same-origin\r\n",
        scripts ? "script-src 'self'; " : "", v6 ? "" : "; form-action 'self'", cb[0] ? " " : "", cb);
    return n < 0 || (size_t)n >= cap ? -1 : 0;
}
