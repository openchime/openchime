/*
 * The daemon as the MCP endpoint's OAuth authorization server, its pure parts
 * (docs/details/AUTH.md §8.13): the two metadata documents, a client's
 * registration and its metadata document checked, the authorize and token
 * requests read, PKCE, the tokens and the eight-letter approval code, the
 * authorize page and the redirect back. No I/O, no database, no globals: the
 * event loop (daemon/netloop.c) serves, the writer (daemon/dbwriter.c) keeps
 * the state, and both build with what is here.
 *
 * A person's client is approved in their signed-in OpenChime app with the code
 * the page shows; the page signs nobody in and runs no script.
 */
#ifndef OC_OAUTH_H
#define OC_OAUTH_H

#include <stddef.h>
#include <stdint.h>

#include "mcp.h"   /* oc_mcp_buf */

/* Every secret is a prefix and 32 random bytes in base64url; the prefixes
 * tell them apart from each other and from an agent's token ("ocpt_"). */
#define OC_OAUTH_ACCESS_PREFIX   "ocpa_"   /* an access token: an hour */
#define OC_OAUTH_REFRESH_PREFIX  "ocpr_"   /* a refresh token: the connection's lifetime */
#define OC_OAUTH_CODE_PREFIX     "ocac_"   /* an authorization code: once, within a minute */
#define OC_OAUTH_REQUEST_PREFIX  "ocrq_"   /* the authorize page's own handle */
#define OC_OAUTH_CLIENT_PREFIX   "occl_"   /* a registered client's id: not a secret */
#define OC_OAUTH_TOKEN_MAX       64        /* the longest: a prefix, 43 characters, room */

#define OC_OAUTH_ACCESS_TTL_MS   3600000u        /* one hour */
#define OC_OAUTH_PENDING_TTL_MS  600000u         /* an approval waits ten minutes */
#define OC_OAUTH_CODE_TTL_MS     60000u          /* an authorization code, one minute */
#define OC_OAUTH_PAGE_REFRESH_S  3u              /* the authorize page reloads itself */
#define OC_OAUTH_DOC_MAX         65536u          /* a client metadata document's bytes */
#define OC_OAUTH_DOC_TIMEOUT_MS  5000            /* ...and how long its fetch may take */
#define OC_OAUTH_DOC_TTL_MS      86400000u       /* ...and how long it is kept */
#define OC_OAUTH_CODE_LEN        8               /* the approval code's letters */
#define OC_OAUTH_NAME_MAX        100             /* a client's name, bytes */
#define OC_OAUTH_URI_MAX         512             /* a redirect URI or client id, bytes */
#define OC_OAUTH_URIS_MAX        8               /* redirect URIs one client may declare */
#define OC_OAUTH_STATE_MAX       512             /* the client's state, bytes */

#define OC_OAUTH_RESOURCE_PATH   "/.well-known/oauth-protected-resource"
#define OC_OAUTH_SERVER_PATH     "/.well-known/oauth-authorization-server"
#define OC_OAUTH_AUTHORIZE_PATH  "/oauth/authorize"
#define OC_OAUTH_TOKEN_PATH      "/oauth/token"
#define OC_OAUTH_REGISTER_PATH   "/oauth/register"

/* The workspace's origin from a request's Host: "https://<host>". 0, or -1
 * when the host holds anything a host name or an address with a port cannot. */
int oc_oauth_base(const char *host, size_t len, char *out, size_t cap);

/* The documents, for the origin `base`. */
void oc_oauth_resource_metadata(oc_mcp_buf *b, const char *base);
void oc_oauth_server_metadata(oc_mcp_buf *b, const char *base);

/* A redirect URI a client may declare: https with a host, or http to a
 * loopback address (127.0.0.1, [::1], localhost); never a fragment, a space
 * or a control character. 1 or 0. */
int oc_oauth_redirect_ok(const char *uri);
/* Whether `uri` is among `lines` (the declared URIs, one to a line): the same
 * string, or for a loopback http URI the same but for its port (RFC 8252
 * §7.3, a native client's port is its own to pick). 1 or 0. */
int oc_oauth_uris_has(const char *lines, const char *uri);
/* A client id that is a Client ID Metadata Document's URL: https, a host and
 * a path, no fragment; with `allow_http_loopback`, a test's http://127.0.0.1
 * too. 1 or 0. */
int oc_oauth_client_id_is_url(const char *id, int allow_http_loopback);

/* A client as it identified itself. */
typedef struct {
    char   name[OC_OAUTH_NAME_MAX + 1];
    char   uris[OC_OAUTH_URIS_MAX][OC_OAUTH_URI_MAX + 1];
    size_t n_uris;
} oc_oauth_client_meta;

/* The declared URIs as one string, a line each. */
void oc_oauth_uris_join(const oc_oauth_client_meta *m, char *out, size_t cap);

/* A registration request's body (RFC 7591), or a metadata document fetched
 * from `url` (its `client_id` must be `url`). 0 with `out` filled, or -1 with
 * `err` naming what is wrong, in words for the client. */
int oc_oauth_parse_registration(const char *body, size_t len, oc_oauth_client_meta *out, char *err, size_t errcap);
int oc_oauth_parse_document(const char *body, size_t len, const char *url, int allow_http_loopback,
                            oc_oauth_client_meta *out, char *err, size_t errcap);
/* A registration's answer. */
void oc_oauth_registered(oc_mcp_buf *b, const char *client_id, const oc_oauth_client_meta *m, uint64_t issued_s);

/* A form-encoded (application/x-www-form-urlencoded, or a query) field,
 * percent-decoded into `out`: 0; -1 absent; -2 repeated, too long or not
 * decodable. */
int oc_oauth_form_get(const char *form, size_t len, const char *key, char *out, size_t cap);

/* An authorize request (GET /oauth/authorize's query). A request carrying
 * `request` (and `code`, the code it shows) is the page reloading itself: the
 * daemon keeps the code hashed only, so the page carries it. */
typedef struct {
    char client_id[OC_OAUTH_URI_MAX + 1];
    char redirect_uri[OC_OAUTH_URI_MAX + 1];
    char challenge[48];
    char state[OC_OAUTH_STATE_MAX + 1];
    char resource[OC_OAUTH_URI_MAX + 1];
    char request[OC_OAUTH_TOKEN_MAX];
    char code[OC_OAUTH_CODE_LEN + 1];   /* the reload's: the code the page shows */
} oc_oauth_authorize;

/* Read and check an authorize request against this workspace's resource
 * (`<base>/mcp`). 0, or -1 with `err` the sentence the page shows. */
int oc_oauth_parse_authorize(const char *query, size_t len, const char *resource, oc_oauth_authorize *out,
                             char *err, size_t errcap);

typedef enum { OC_OAUTH_GRANT_CODE = 1, OC_OAUTH_GRANT_REFRESH } oc_oauth_grant;
typedef struct {
    oc_oauth_grant grant;
    char code[OC_OAUTH_TOKEN_MAX];
    char verifier[132];
    char redirect_uri[OC_OAUTH_URI_MAX + 1];
    char client_id[OC_OAUTH_URI_MAX + 1];
    char resource[OC_OAUTH_URI_MAX + 1];
    char refresh[OC_OAUTH_TOKEN_MAX];
} oc_oauth_token_req;

/* Read a token request's form body. NULL, or the OAuth error code to answer
 * ("invalid_request", "unsupported_grant_type"). */
const char *oc_oauth_parse_token(const char *form, size_t len, oc_oauth_token_req *out);

/* PKCE S256 (RFC 7636): `verifier` 43 to 128 unreserved characters whose
 * SHA-256, in base64url, is `challenge`. 1 or 0. */
int oc_oauth_pkce_ok(const char *verifier, const char *challenge);

/* A new secret: `prefix` and 32 random bytes in base64url. 0, or -1. */
int oc_oauth_mint(const char *prefix, char *out, size_t cap);
/* The SHA-256 a secret is stored as: of the whole string, prefix and all. */
int oc_oauth_hash(const char *secret, size_t len, uint8_t out[32]);

typedef enum { OC_OAUTH_TOKEN_OTHER = 0, OC_OAUTH_TOKEN_AGENT, OC_OAUTH_TOKEN_PERSON } oc_oauth_token_kind;
/* Which a bearer token is, by its prefix. */
oc_oauth_token_kind oc_oauth_token_of(const char *tok, size_t len);

/* The approval code: eight letters from the device codes' alphabet. */
int oc_oauth_approval_code(char out[OC_OAUTH_CODE_LEN + 1]);
/* A code as a person typed it -- either case, a dash or spaces anywhere -- as
 * its eight letters. 0, or -1 when it is not one. */
int oc_oauth_code_normalize(const char *in, char out[OC_OAUTH_CODE_LEN + 1]);

/* A connection's lifetime in days: never (0), 30, 90 or 365. 1 or 0. */
int oc_oauth_lifetime_ok(uint32_t days);

/* The authorize page. */
typedef enum {
    OC_OAUTH_PAGE_PENDING = 1,   /* name the client, show the code, reload itself */
    OC_OAUTH_PAGE_EXPIRED,       /* the request waited too long */
    OC_OAUTH_PAGE_REFUSED,       /* `message` says why */
    OC_OAUTH_PAGE_OFF            /* connections are off on this workspace */
} oc_oauth_page_kind;
/* `reload` is the path and query the pending page reloads to. */
void oc_oauth_page(oc_mcp_buf *b, oc_oauth_page_kind kind, const char *client_name, const char *code,
                   const char *reload, const char *message);

/* The redirect back to the client: `redirect_uri` with `code` (or `error`),
 * `state` and `iss` (RFC 9207) added to its query. */
void oc_oauth_redirect(oc_mcp_buf *b, const char *redirect_uri, const char *code, const char *error,
                       const char *state, const char *iss);

/* The token endpoint's answers. */
void oc_oauth_token_json(oc_mcp_buf *b, const char *access, const char *refresh, unsigned expires_in);
void oc_oauth_error_json(oc_mcp_buf *b, const char *error, const char *description);

#endif /* OC_OAUTH_H */
