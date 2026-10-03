/*
 * The daemon's own sign-in pages (AUTH.md §8.10): sign in, sign up with an
 * invitation, change a password. Only the HTML is here -- escaped, self-contained,
 * no scripts, no external fetches, no cookies -- and the headers every page
 * carries; the event loop does the requests and the writer the checks.
 *
 * Every URL a page uses is RELATIVE, so the same pages work whether the browser
 * reached them directly (https://<workspace>/signin) or through a client's
 * loopback tunnel (http://127.0.0.1:<port>/p/<secret>/signin).
 */
#ifndef OPENCHIME_WEBPAGES_H
#define OPENCHIME_WEBPAGES_H

#include <stddef.h>

typedef enum { OC_PAGE_SIGNIN, OC_PAGE_SIGNUP, OC_PAGE_PASSWORD, OC_PAGE_DEVICE, OC_PAGE_STEP,
               OC_PAGE_SECURITY, OC_PAGE_RESET } oc_page_kind;

typedef enum {
    OC_SEC_SIGNIN = 0,   /* username and password, to begin */
    OC_SEC_SETUP,        /* the key and QR code, and the first code */
    OC_SEC_CONFIRM,      /* the first code again, after a wrong one */
    OC_SEC_CODES,        /* on: the recovery codes, once */
    OC_SEC_ON,           /* already on: a code turns it off */
    OC_SEC_OFF,          /* turned off */
    OC_SEC_PASSKEY,      /* on, the code given: a passkey to add */
    OC_SEC_PASSKEY_DONE  /* a passkey added */
} oc_sec_screen;

typedef struct {
    oc_page_kind kind;
    const char  *redirect_uri;   /* SIGNIN/SIGNUP: where the token goes; "" otherwise */
    const char  *nonce;          /* SIGNIN/SIGNUP: the client's PKCE challenge */
    const char  *username;       /* prefilled, or "" */
    const char  *invite;         /* SIGNUP: the invitation, prefilled, or "" */
    const char  *message;        /* why the form is back, or "" */
    int          done;           /* PASSWORD: it was changed. DEVICE: approved */
    /* DEVICE (AUTH.md §8.11): the code, once one is found -- "" asks for it --
     * where the request came from and how long ago, and whether it was denied. */
    const char  *user_code;
    const char  *from;
    unsigned     minutes_ago;
    int          denied;
    /* STEP (AUTH.md §8.6): the ticket the password page earned, and where the
     * code is posted -- relative to the page it answers, which is not always
     * at the same depth. */
    const char  *ticket;
    const char  *action;
    /* SECURITY (AUTH.md §8.6): which of its screens -- and, setting one up, the
     * key to type and the otpauth URI its QR code holds; done, the recovery
     * codes, one per line. */
    oc_sec_screen sec;
    const char  *secret;
    const char  *otpauth;
    const char  *codes;
    /* RESET (AUTH.md §2): the one-time link's token. */
    const char  *reset;
    /* A passkey (AUTH.md §8.6), offered only on the workspace's own trusted
     * name: 1 to answer with one (STEP), 2 to add one (SECURITY's PASSKEY
     * screen); the ceremony's challenge and relying party, the credentials
     * already there (base64url, comma-separated), and the person's handle and
     * name for a new one. */
    int          pk_mode;
    const char  *pk_challenge, *pk_rp, *pk_creds, *pk_user, *pk_name;
} oc_page;

/* The page, malloc'd, its length in *len. NULL on no memory. */
char *oc_page_render(const oc_page *p, size_t *len);

/* The page for a link that is not a sign-in's (no loopback redirect, no
 * challenge), or for a workspace whose accounts are not local. Static. */
const char *oc_page_invalid(size_t *len);
const char *oc_page_unavailable(size_t *len);

/* The headers every page carries (http.h's `extra`), into `out`: no framing,
 * no caching, no referrer, and a policy that runs nothing and posts forms only
 * to the page's own origin -- and, for a sign-in, to the client's loopback
 * callback, which the post's redirect lands on (browsers hold a form's redirect
 * to form-action too). `redirect_uri` may be NULL. Returns 0, -1 if `cap` is
 * too small. */
int oc_page_headers(const char *redirect_uri, char *out, size_t cap);
/* The same, for a page that runs a script the daemon serves (a passkey's, or
 * the recovery codes' copy button: `script-src 'self'`) when `scripts`. */
int oc_page_headers_ex(const char *redirect_uri, int scripts, char *out, size_t cap);

/* The passkey script (AUTH.md §8.6), served at /webauthn.js, and its SRI
 * value ("sha256-<base64>") for the page that loads it. */
const char *oc_webauthn_js(size_t *len);
const char *oc_webauthn_js_integrity(void);
/* The recovery codes' copy script (AUTH.md §8.6), served at /codes.js, and its
 * SRI value. */
const char *oc_codes_js(size_t *len);
const char *oc_codes_js_integrity(void);

/* `in`/`n` with &, <, >, " and ' escaped, appended at `*o` in `out`. -1 if it
 * does not fit. */
int oc_html_escape(char *out, size_t cap, size_t *o, const char *in, size_t n);

#endif /* OPENCHIME_WEBPAGES_H */
