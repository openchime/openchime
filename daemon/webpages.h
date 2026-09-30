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

typedef enum { OC_PAGE_SIGNIN, OC_PAGE_SIGNUP, OC_PAGE_PASSWORD } oc_page_kind;

typedef struct {
    oc_page_kind kind;
    const char  *redirect_uri;   /* SIGNIN/SIGNUP: where the token goes; "" otherwise */
    const char  *nonce;          /* SIGNIN/SIGNUP: the client's PKCE challenge */
    const char  *username;       /* prefilled, or "" */
    const char  *invite;         /* SIGNUP: the invitation, prefilled, or "" */
    const char  *message;        /* why the form is back, or "" */
    int          done;           /* PASSWORD: it was changed */
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

/* `in`/`n` with &, <, >, " and ' escaped, appended at `*o` in `out`. -1 if it
 * does not fit. */
int oc_html_escape(char *out, size_t cap, size_t *o, const char *in, size_t n);

#endif /* OPENCHIME_WEBPAGES_H */
