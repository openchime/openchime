/* The configuration registry (config_table.h): every environment variable the
 * daemon reads, and the few the clients read, one row each. Adding a setting
 * is adding a row; the loader, CONFIG.md's reference, the packaged environment
 * file and --check-config follow from it. The order here is the order of the
 * reference, grouped by `group`.
 *
 * A row earns its place by being something an operator must decide that the
 * daemon cannot derive. What the control plane writes is CFG_MANAGED; what
 * only a test varies is CFG_DEMO; everything else is a constant (config.h). */
#include "config_table.h"
#include "protocol.h"   /* the wire's caps */

#include <stddef.h>

/* Structured values, parsed in config.c. */
int cfg_parse_auth(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap);
int cfg_parse_tls(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap);
int cfg_parse_s3(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap);
int cfg_parse_summary(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap);
int cfg_parse_connect(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap);
int cfg_parse_token_prefix(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap);
int cfg_parse_trusted_proxies(oc_config *c, const cfg_row *r, const char *name, const char *v, char *err, size_t errcap);

#define F(field) offsetof(oc_config, field)
#define STR(n, cls, grp, d, doc, ...)   { n, CFG_STR,  cls, grp, d, doc, 0, 0, NULL, __VA_ARGS__ }
#define PATHR(n, cls, grp, d, doc, ...) { n, CFG_PATH, cls, grp, d, doc, 0, 0, NULL, __VA_ARGS__ }
#define INT(n, cls, grp, d, lo, hi, doc, fld)  { n, CFG_INT,  cls, grp, d, doc, lo, hi, NULL, F(fld), NULL, 0, 0 }
#define U64(n, cls, grp, d, lo, hi, doc, fld)  { n, CFG_U64,  cls, grp, d, doc, lo, hi, NULL, F(fld), NULL, 0, 0 }
#define BOOL(n, cls, grp, d, doc, fld)         { n, CFG_BOOL, cls, grp, d, doc, 0, 0, NULL, F(fld), NULL, 0, 0 }
#define ENUM(n, cls, grp, d, vals, doc, fld)   { n, CFG_ENUM, cls, grp, d, doc, 0, 0, vals, F(fld), NULL, 0, 0 }
#define CUSTOM(n, cls, grp, d, doc, fn, flags) { n, CFG_CUSTOM, cls, grp, d, doc, 0, 0, NULL, 0, fn, flags, 0 }
/* A client's variable: known and documented, never stored by the daemon. */
#define CLIENT(n, cls, grp, doc) { n, CFG_STR, cls, grp, NULL, doc, 0, 0, NULL, 0, NULL, CFG_CLIENT, 0 }

#define G_SET    "Settings"
#define G_FLAG   "Features"
#define G_MAN    "Managed"
#define G_DEMO   "Demo"

const cfg_row OC_CFG[] = {
    /* --- Settings: no feature behind them, a value an operator chooses -------- */
    STR("OPENCHIME_WORKSPACE_NAME", CFG_OPERATOR, G_SET, "", "The workspace's display name, shown to clients; unset, a client derives one from the host.", F(workspace_name), NULL, 0, 0),
    PATHR("OPENCHIME_DATA_DIR", CFG_OPERATOR, G_SET, "/data", "Where the daemon keeps everything: `openchime.db`, `blobs/`, `cert.pem`, `key.pem`, `factor.key`, `summary/`. The packages set `/var/lib/openchime`.", F(data_dir), NULL, 0, 0),
    INT("OPENCHIME_PORT", CFG_OPERATOR, G_SET, "8443", 1, 65535, "The TLS port the protocol, the web client and the sign-in pages are served on, and the UDP port calls use (443 for a real deployment, ARCH-54).", proto_port),
    INT("OPENCHIME_HEALTH_PORT", CFG_OPERATOR, G_SET, "8080", 0, 65535, "The plaintext port for `/healthz` and the landing page (ARCH-25); 0 opens none.", health_port),
    STR("OPENCHIME_JOIN", CFG_OPERATOR, G_SET, NULL, "Who may join by signing in through a provider: `domain:`, `tenant:`, `subject:` and `owner:` rules, comma-separated (AUTH.md §3). Unset: only by invitation.", F(oidc.allow), NULL, 0, 0),
    { "OPENCHIME_OIDC_CONNECT_%d", CFG_CUSTOM, CFG_OPERATOR, G_SET, NULL,
      "A direct connection to your own provider, 1 to 4 (SSO.md): `label=…;issuer=https://…;client_id=…;secret_file=…;subject=sub|oid;redirect=127.0.0.1|localhost`.",
      0, 0, NULL, 0, cfg_parse_connect, CFG_PATTERN, OC_OIDC_MAX_CONNECT },
    PATHR("OPENCHIME_EXTRA_CA", CFG_OPERATOR, G_SET, NULL, "PEM roots trusted beside the built-in ones for every outbound connection (push, enrollment, unfurls, S3): a service behind a private CA.", F(extra_ca), NULL, CFG_MUST_EXIST, 0),
    CUSTOM("OPENCHIME_TRUSTED_PROXIES", CFG_OPERATOR, G_SET, NULL,
           "Addresses or CIDRs whose PROXY protocol v2 header is believed, comma-separated; the connection's address is then the one the proxy saw.",
           cfg_parse_trusted_proxies, 0),

    /* --- Features: each turns a capability on or off, with its one value ------- */
    CUSTOM("OPENCHIME_TLS", CFG_OPERATOR, G_FLAG, "self",
           "The certificate the daemon presents (TLS.md): `self` (its own, made at first run), `file` (`cert.pem` and `key.pem` in the data directory), `acme:<names>[;email=…][;directory=…]` (RFC 8555 for the comma-separated names), or `central` (through the control plane; the default on a managed box).",
           cfg_parse_tls, 0),
    CUSTOM("OPENCHIME_AUTH", CFG_OPERATOR, G_FLAG, "local",
           "Where accounts come from: `local` (passwords held here), `relay` (sign-in through the control plane this box is enrolled with, ARCH-56), or `local,relay`.",
           cfg_parse_auth, 0),
    ENUM("OPENCHIME_MFA", CFG_OPERATOR, G_FLAG, "optional", "off|optional|required", "Whether local accounts are asked for a second step (AUTH.md §8.6).", local_mfa),
    CUSTOM("OPENCHIME_S3", CFG_OPERATOR, G_FLAG, NULL,
           "Attachments in S3 instead of under the data directory (ARCH-70): `s3://KEY:SECRET@host[:port]/bucket[?region=…]`, or `http+s3://` for a plaintext endpoint; percent-encode `/`, `+` and `@` in the key and secret. Unset: local disk.",
           cfg_parse_s3, CFG_SECRET),
    BOOL("OPENCHIME_TTS", CFG_OPERATOR, G_FLAG, "on", "Read-aloud (ARCH-111), in a build that carries it; off saves the voice model's memory.", tts.enabled),
    BOOL("OPENCHIME_STT", CFG_OPERATOR, G_FLAG, "on", "Voice input (ARCH-112), in a build that carries it.", stt.enabled),
    CUSTOM("OPENCHIME_SUMMARY", CFG_OPERATOR, G_FLAG, "off",
           "Summaries (REQ-310): `off`, `local` (the model runs inside the daemon, fetched on first start), or `cloud:<url>,<model>` (a hosted API's base URL and the model there).",
           cfg_parse_summary, 0),
    STR("OPENCHIME_SUMMARY_API_KEY", CFG_OPERATOR, G_FLAG, "", "The hosted API's key, for `cloud`.", F(summary.api_key), NULL, CFG_SECRET, 0),
    STR("OPENCHIME_ENROLL_URL", CFG_OPERATOR, G_FLAG, NULL, "The control plane to enrol with (ARCH-84), which makes the box federated: https, or http to loopback for a test. Prints an `oce1.` code to the log on first run.", F(enroll.url), NULL, 0, 0),
    STR("OPENCHIME_PUSH_URL", CFG_OPERATOR, G_FLAG, NULL, "The mobile push relay (ARCH-16/85), contentless; needs an enrollment. https, or http to loopback for a test.", F(push.url), NULL, 0, 0),
    ENUM("OPENCHIME_INVITE_MAIL", CFG_OPERATOR, G_FLAG, "off", "off|on", "Whether invitations bound to an address are reported to central for it to mail (REQ-280); needs an active enrollment.", invite_mail),

    /* --- Managed: the control plane's, at provision time (MANAGED.md) ---------- */
    STR("OPENCHIME_MANAGED_TICKET", CFG_MANAGED, G_MAN, NULL, "The one-time claim ticket; with it the box is managed and the other rows here may be set.", F(enroll.ticket), NULL, CFG_SECRET, 0),
    STR("OPENCHIME_MANAGED_AUDIENCE", CFG_MANAGED, G_MAN, NULL, "The audience central minted for this workspace; the relay's tokens carry it.", F(oidc.audience), NULL, 0, 0),
    STR("OPENCHIME_MANAGED_RELAY_KEY", CFG_MANAGED, G_MAN, NULL, "A relay public key, PEM, pinned beneath the keys the relay publishes (AUTH.md §3.3).", F(oidc.pubkey), NULL, 0, 0),
    INT("OPENCHIME_MANAGED_MAX_USERS", CFG_MANAGED, G_MAN, "0", 0, 1000000, "The most registered users (CP-7); 0 is unlimited.", max_users),
    STR("OPENCHIME_MANAGED_ADDRESS", CFG_MANAGED, G_MAN, "", "The workspace's canonical address, when it moved and still answers at the old one; clients re-file it under this.", F(workspace_address), NULL, 0, 0),
    INT("OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT", CFG_MANAGED, G_MAN, "0", 0, 65535, "The UDP port clients are told, where a front door maps it (AUDIO.md §4); 0 is the bound one.", audio_advertise_port),
    CUSTOM("OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX", CFG_MANAGED, G_MAN, NULL,
           "Hex bytes (at most 16) that lead every relay token, so a front door can route by them.",
           cfg_parse_token_prefix, 0),

    /* --- Demo builds only -------------------------------------------------------- */
    STR("OPENCHIME_DEMO_USERS", CFG_DEMO, G_DEMO, NULL, "Accounts made at first start, `name:password:role,…`, for a fixture; a real deployment's first owner comes from the setup token.", F(demo.users), NULL, CFG_SECRET, 0),
    INT("OPENCHIME_DEMO_FILE_PAGE", CFG_DEMO, G_DEMO, "200", 1, (int64_t)OC_MAX_FILE_LIST, "How many files one LIST_FILES page carries, so a test sees a second page.", file_page),
    INT("OPENCHIME_DEMO_CALL_MAX", CFG_DEMO, G_DEMO, "10", 2, (int64_t)OC_MAX_CALL_PARTICIPANTS, "The most people in one call, so a test can fill one.", call_max),
    INT("OPENCHIME_DEMO_CONNS_PER_IP", CFG_DEMO, G_DEMO, "1024", 0, 1000000, "Connections one address may hold; 0 is no cap, for a load test from one host.", max_conns_per_ip),
    U64("OPENCHIME_DEMO_MAINT_MS", CFG_DEMO, G_DEMO, "300000", 1000, 86400000, "How often the maintenance pass runs.", storage.interval_ms),
    U64("OPENCHIME_DEMO_SCHED_MS", CFG_DEMO, G_DEMO, "15000", 50, 3600000, "How often scheduled messages are swept.", sched_tick_ms),
    BOOL("OPENCHIME_DEMO_PRIVATE_UNFURL", CFG_DEMO, G_DEMO, "off", "Turns the unfurl fetcher's SSRF gate off, so a test can fetch a loopback fixture.", demo.private_unfurl),
    PATHR("OPENCHIME_DEMO_OS_ROOTS", CFG_DEMO, G_DEMO, NULL, "The CA bundle read instead of the operating system's, for a test with its own CA.", F(demo.os_roots), NULL, 0, 0),
    CLIENT("OPENCHIME_TEST_DIR", CFG_DEMO, G_DEMO, "GUI: the directory of the automation hook's command channel and dumps (TESTING.md)."),
    CLIENT("OPENCHIME_TEST_AUDIO", CFG_DEMO, G_DEMO, "Clients: `synthetic` audio devices, or `mic-denied`."),
    CLIENT("OPENCHIME_TEST_MIC", CFG_DEMO, G_DEMO, "Clients: a WAV the synthetic microphone speaks."),
    CLIENT("OPENCHIME_TEST_MIC_LEAD_MS", CFG_DEMO, G_DEMO, "Clients: silence before the WAV, in ms."),
    CLIENT("OPENCHIME_TEST_TONE", CFG_DEMO, G_DEMO, "Clients: the synthetic microphone's tone in Hz (20 to 7000; 440 unset)."),
    CLIENT("OPENCHIME_TEST_MIC_ECHO", CFG_DEMO, G_DEMO, "Clients: the speaker echoed into the microphone (`1`), or alone (`only`)."),
    CLIENT("OPENCHIME_TEST_CAPTURE", CFG_DEMO, G_DEMO, "Clients: `synthetic` camera and screen, `synthetic-camera`, or `denied`."),
    CLIENT("OPENCHIME_TEST_SCREEN_GONE_MS", CFG_DEMO, G_DEMO, "Clients: the synthetic screen ends after this many ms."),
    CLIENT("OPENCHIME_TEST_VIDEO_CAP_MS", CFG_DEMO, G_DEMO, "Clients: a shorter video message cap, in ms."),
    CLIENT("OPENCHIME_TEST_CALL_TIMERS", CFG_DEMO, G_DEMO, "Clients: `probe:<ms>,udp_wait:<ms>,udp_lost:<ms>,tcp_probe:<ms>`, any subset."),
    CLIENT("OPENCHIME_TEST_SUFFIX", CFG_DEMO, G_DEMO, "Clients: the DNS suffix appended to a bare workspace name instead of the service's (ARCH-14)."),
    CLIENT("OPENCHIME_TEST_SIGNIN", CFG_DEMO, G_DEMO, "Web client: `user:password` the harness types into the daemon's sign-in page for the person (TESTING.md)."),

    { NULL, CFG_STR, CFG_OPERATOR, NULL, NULL, NULL, 0, 0, NULL, 0, NULL, 0, 0 }
};

/* Every name that was a setting and is not: the replacement, or the constant
 * it became. A production value is in config.h. */
const cfg_removed OC_CFG_REMOVED[] = {
    { "OPENCHIME_DEPLOYMENT_MODE",   "derived: managed with OPENCHIME_MANAGED_TICKET, federated with OPENCHIME_ENROLL_URL, else standalone" },
    { "OPENCHIME_DB_PATH",           "the database is openchime.db under OPENCHIME_DATA_DIR" },
    { "OPENCHIME_BLOB_DIR",          "attachments are blobs/ under OPENCHIME_DATA_DIR" },
    { "OPENCHIME_TLS_CERT",          "cert.pem under OPENCHIME_DATA_DIR" },
    { "OPENCHIME_TLS_KEY",           "key.pem under OPENCHIME_DATA_DIR" },
    { "OPENCHIME_FACTOR_KEY_FILE",   "factor.key under OPENCHIME_DATA_DIR" },
    { "OPENCHIME_TTS_DATA_DIR",      "the voice data is found at /usr/share/openchime/tts, else tts/ beside the executable" },
    { "OPENCHIME_STT_DATA_DIR",      "the recognizer's data is found at /usr/share/openchime/stt, else stt/ beside the executable" },
    { "OPENCHIME_PROTO_PORT",        "renamed OPENCHIME_PORT" },
    { "OPENCHIME_AUDIO_PORT",        "the call relay's UDP port is OPENCHIME_PORT" },
    { "OPENCHIME_WORKSPACE_ADDRESS", "renamed OPENCHIME_MANAGED_ADDRESS (the control plane's)" },
    { "OPENCHIME_MAX_USERS",         "renamed OPENCHIME_MANAGED_MAX_USERS (the control plane's)" },
    { "OPENCHIME_MAX_CONNS_PER_IP",  "a constant, 1024; a demo build reads OPENCHIME_DEMO_CONNS_PER_IP" },
    { "OPENCHIME_XFER_WORKERS",      "a constant, 2" },
    { "OPENCHIME_SESSION_DAYS",      "a constant, 30" },
    { "OPENCHIME_SESSION_IDLE_DAYS", "a constant: no idle limit" },
    { "OPENCHIME_LOCAL_MFA",         "renamed OPENCHIME_MFA" },
    { "OPENCHIME_AUTH_MODE",         "renamed OPENCHIME_AUTH" },
    { "OPENCHIME_OIDC_ISSUER",       "the relay is the control plane: its issuer is OPENCHIME_ENROLL_URL's origin" },
    { "OPENCHIME_OIDC_AUDIENCE",     "an enrolled box's audience is its enrollment's; a managed box is started with OPENCHIME_MANAGED_AUDIENCE" },
    { "OPENCHIME_OIDC_PUBKEY",       "the relay's keys are the ones it publishes; a managed box may pin one with OPENCHIME_MANAGED_RELAY_KEY" },
    { "OPENCHIME_OIDC_PUBKEY_FILE",  "the relay's keys are the ones it publishes" },
    { "OPENCHIME_OIDC_ALLOW",        "renamed OPENCHIME_JOIN" },
    { "OPENCHIME_OIDC_EMAIL_LINK",   "a constant: an emailed code signs into a person a provider knows only through that provider" },
    { "OPENCHIME_TLS_SOURCE",        "OPENCHIME_TLS=self|file|acme:<names>|central" },
    { "OPENCHIME_TLS_NAME",          "OPENCHIME_TLS=acme:<names>" },
    { "OPENCHIME_ACME_DIRECTORY",    "OPENCHIME_TLS=acme:<names>;directory=<url>" },
    { "OPENCHIME_ACME_EMAIL",        "OPENCHIME_TLS=acme:<names>;email=<address>" },
    { "OPENCHIME_MAX_ATTACHMENT_SIZE",    "a constant: the wire's cap, 200 MiB" },
    { "OPENCHIME_MAX_VIDEO_MESSAGE_SIZE", "a constant, 160 MiB" },
    { "OPENCHIME_S3_ENDPOINT",       "OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…]" },
    { "OPENCHIME_S3_BUCKET",         "OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…]" },
    { "OPENCHIME_S3_ACCESS_KEY",     "OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…]" },
    { "OPENCHIME_S3_SECRET_KEY",     "OPENCHIME_S3=s3://KEY:SECRET@host[:port]/bucket[?region=…]" },
    { "OPENCHIME_S3_REGION",         "OPENCHIME_S3=s3://…/bucket?region=<region>" },
    { "OPENCHIME_ATTACH_MAX_AGE_DAYS", "a constant: attachments are kept" },
    { "OPENCHIME_EVICT",             "a constant: on, on local disk" },
    { "OPENCHIME_EVICT_GRACE_HOURS", "a constant, 24" },
    { "OPENCHIME_DB_RESERVE_MB",     "a constant, 256" },
    { "OPENCHIME_PRESSURE_MB",       "a constant, 512" },
    { "OPENCHIME_MAINT_BATCH",       "a constant, 64" },
    { "OPENCHIME_AUDIT_MAX_DAYS",    "a constant, 365" },
    { "OPENCHIME_MAINT_INTERVAL_MS", "a constant, five minutes; a demo build reads OPENCHIME_DEMO_MAINT_MS" },
    { "OPENCHIME_SCHED_TICK_MS",     "a constant, fifteen seconds; a demo build reads OPENCHIME_DEMO_SCHED_MS" },
    { "OPENCHIME_ENROLL_CODE_FILE",  "the enrollment code is in the log" },
    { "OPENCHIME_ENROLL_WAIT_SECS",  "a constant: one attempt at first start, then each boot; a managed box waits two minutes for its claim" },
    { "OPENCHIME_ENROLL_TICKET",     "renamed OPENCHIME_MANAGED_TICKET" },
    { "OPENCHIME_AUDIO_ADVERTISE_PORT", "renamed OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT (the control plane's)" },
    { "OPENCHIME_AUDIO_TOKEN_PREFIX",   "renamed OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX (the control plane's)" },
    { "OPENCHIME_CALL_MAX",          "a constant, 10; a demo build reads OPENCHIME_DEMO_CALL_MAX" },
    { "OPENCHIME_TTS_LANG",          "the binary carries one language" },
    { "OPENCHIME_TTS_QUEUE",         "a constant, 256" },
    { "OPENCHIME_TTS_IDLE_SECS",     "a constant, 300" },
    { "OPENCHIME_TTS_RATE",          "a constant, 60 a minute" },
    { "OPENCHIME_STT_QUEUE",         "a constant, 64" },
    { "OPENCHIME_STT_IDLE_SECS",     "a constant, 300" },
    { "OPENCHIME_STT_RATE",          "a constant, 60 a minute" },
    { "OPENCHIME_STT_MAX_SECS",      "a constant, 30: the model's limit" },
    { "OPENCHIME_SUMMARY_URL",       "OPENCHIME_SUMMARY=cloud:<url>,<model>" },
    { "OPENCHIME_SUMMARY_MODEL",     "OPENCHIME_SUMMARY=cloud:<url>,<model>" },
    { "OPENCHIME_TEST_PASSWORD_AUTH",    "removed: a password is taken only on the sign-in page; tests sign in through it (tests/testsignin.h)" },
    { "OPENCHIME_DEMO_PASSWORD_FRAMES",  "removed: a password is taken only on the sign-in page; tests sign in through it (tests/testsignin.h)" },
    { "OPENCHIME_BOOTSTRAP_USERS",       "renamed OPENCHIME_DEMO_USERS" },
    { "OPENCHIME_FILE_PAGE",             "renamed OPENCHIME_DEMO_FILE_PAGE" },
    { "OPENCHIME_UNFURL_ALLOW_PRIVATE",  "renamed OPENCHIME_DEMO_PRIVATE_UNFURL" },
    { "OPENCHIME_TEST_OS_ROOTS",         "renamed OPENCHIME_DEMO_OS_ROOTS" },
    { "OPENCHIME_SUFFIX",            "the service suffix is built in; a demo client reads OPENCHIME_TEST_SUFFIX" },
    { "OPENCHIME_RECOVER_MB",        "nothing read it" },
    { "OPENCHIME_BLOB_BACKEND",      "OPENCHIME_S3 set is S3; unset is local disk" },
    { "OPENCHIME_STATE",             "the TUI keeps its state in the credential store (ARCH-88)" },
    { NULL, NULL }
};
