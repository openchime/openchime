/* The configuration registry (config_table.h): every environment variable the
 * daemon reads, and the few the clients read, one row each. Adding a setting
 * is adding a row; the loader, CONFIG.md's reference, the packaged environment
 * file and --check-config follow from it. The order here is the order of the
 * reference, grouped by `group`. */
#include "config_table.h"
#include "protocol.h"   /* the wire's caps */

#include <stddef.h>

/* Structured values, parsed in config.c. */
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
/* A client's variable: known and documented, never stored by the daemon. */
#define CLIENT(n, cls, grp, doc) { n, CFG_STR, cls, grp, NULL, doc, 0, 0, NULL, 0, NULL, CFG_CLIENT, 0 }

#define G_CORE   "Core"
#define G_SESS   "Sessions and local sign-in"
#define G_AUTH   "Authentication"
#define G_TLS    "Outbound TLS"
#define G_CERT   "Certificates"
#define G_BLOB   "Attachments and blob storage"
#define G_STOR   "Storage pressure and maintenance"
#define G_FED    "Federated services"
#define G_AUDIO  "Audio"
#define G_TTS    "Read-aloud"
#define G_STT    "Voice input"
#define G_SUM    "Summaries"
#define G_CALLS  "Calls and files"
#define G_CLIENT "Client-side"
#define G_DEMO   "Test switches"

const cfg_row OC_CFG[] = {
    /* --- Core ------------------------------------------------------------- */
    ENUM("OPENCHIME_DEPLOYMENT_MODE", CFG_OPERATOR, G_CORE, "standalone", "standalone|federated|managed",
         "The deployment model (ARCH-76). `federated` enables nothing by itself: each service is gated on its own URL.", deployment_mode),
    STR("OPENCHIME_WORKSPACE_NAME", CFG_OPERATOR, G_CORE, "", "The workspace's display name, shown to clients; unset, a client derives one from the host.", F(workspace_name), NULL, 0, 0),
    STR("OPENCHIME_WORKSPACE_ADDRESS", CFG_OPERATOR, G_CORE, "", "The workspace's canonical address, when it moved and still answers at the old one; clients re-file it under this.", F(workspace_address), NULL, 0, 0),
    PATHR("OPENCHIME_DB_PATH", CFG_OPERATOR, G_CORE, "/data/openchime.db", "The SQLite database; created and migrated by the daemon. The packages set `/var/lib/openchime/openchime.db`.", F(db_path), NULL, 0, 0),
    PATHR("OPENCHIME_TLS_CERT", CFG_OPERATOR, G_CORE, "/data/cert.pem", "The certificate the daemon presents (PEM); with `OPENCHIME_TLS_SOURCE=self`, where it writes its own.", F(tls_cert), NULL, 0, 0),
    PATHR("OPENCHIME_TLS_KEY", CFG_OPERATOR, G_CORE, "/data/key.pem", "Its private key (PEM).", F(tls_key), NULL, 0, 0),
    INT("OPENCHIME_PROTO_PORT", CFG_OPERATOR, G_CORE, "8443", 1, 65535, "The TLS port the protocol, the web client and the sign-in pages are served on (443 for a real deployment, ARCH-54).", proto_port),
    INT("OPENCHIME_HEALTH_PORT", CFG_OPERATOR, G_CORE, "8080", 0, 65535, "The plaintext port for `/healthz` and the landing page (ARCH-25); 0 turns it off.", health_port),
    INT("OPENCHIME_MAX_USERS", CFG_OPERATOR, G_CORE, "0", 0, 1000000, "The most registered users; 0 is unlimited. A managed box has it set by the control plane (CP-7).", max_users),
    INT("OPENCHIME_MAX_CONNS_PER_IP", CFG_TUNING, G_CORE, "256", 0, 1000000, "Connections one address may hold at once; 0 disables the cap. Behind a proxy, see OPENCHIME_TRUSTED_PROXIES.", max_conns_per_ip),
    { "OPENCHIME_TRUSTED_PROXIES", CFG_CUSTOM, CFG_OPERATOR, G_CORE, NULL,
      "Addresses or CIDRs whose PROXY protocol v2 header is believed, comma-separated; the connection's address is then the one the proxy saw.",
      0, 0, NULL, F(trusted_proxies), cfg_parse_trusted_proxies, 0, 0 },
    INT("OPENCHIME_XFER_WORKERS", CFG_TUNING, G_CORE, "2", 1, 16, "Threads that move attachment bytes to and from the blob store (ARCH-69); each holds a TLS session and a chunk buffer.", xfer_workers),

    /* --- Sessions and local sign-in ------------------------------------------ */
    INT("OPENCHIME_SESSION_DAYS", CFG_OPERATOR, G_SESS, "30", 1, 3650, "How many days a session lives (REQ-181).", session_days),
    INT("OPENCHIME_SESSION_IDLE_DAYS", CFG_OPERATOR, G_SESS, "0", 0, 3650, "How many days a session may go unused; 0 is no idle limit.", session_idle_days),
    ENUM("OPENCHIME_LOCAL_MFA", CFG_OPERATOR, G_SESS, "optional", "off|optional|required", "Whether local accounts are asked for a second step (AUTH.md §8.6).", local_mfa),
    PATHR("OPENCHIME_FACTOR_KEY_FILE", CFG_OPERATOR, G_SESS, NULL, "The key local accounts' second-step secrets are sealed under; unset, `factor.key` beside the database. Back it up with the database.", F(factor_key_file_env), NULL, 0, 0),

    /* --- Authentication ------------------------------------------------------ */
    STR("OPENCHIME_AUTH_MODE", CFG_OPERATOR, G_AUTH, "local", "Where accounts come from, comma-separated: `local` (passwords held here), `relay` or `oidc` (the central relay, ARCH-56); checked at startup.", F(auth_mode), NULL, 0, 0),
    STR("OPENCHIME_OIDC_ISSUER", CFG_OPERATOR, G_AUTH, NULL, "The relay's issuer URL, for `relay`.", F(oidc.issuer), NULL, 0, 0),
    STR("OPENCHIME_OIDC_AUDIENCE", CFG_OPERATOR, G_AUTH, NULL, "The audience the relay's tokens carry for this workspace; an enrolled box derives it.", F(oidc.audience), NULL, 0, 0),
    STR("OPENCHIME_OIDC_PUBKEY", CFG_OPERATOR, G_AUTH, NULL, "The relay's public key, PEM, inline. The control plane writes this form.", F(oidc.pubkey), NULL, 0, 0),
    PATHR("OPENCHIME_OIDC_PUBKEY_FILE", CFG_OPERATOR, G_AUTH, NULL, "The relay's public key, PEM, as a file; read at startup and preferred over the inline form.", F(oidc.pubkey_file), NULL, CFG_MUST_EXIST, 0),
    STR("OPENCHIME_OIDC_ALLOW", CFG_OPERATOR, G_AUTH, NULL, "Who may join by signing in: `domain:`, `tenant:`, `subject:` and `owner:` rules, comma-separated (AUTH.md §3). Empty: only by invitation.", F(oidc.allow), NULL, 0, 0),
    ENUM("OPENCHIME_OIDC_EMAIL_LINK", CFG_OPERATOR, G_AUTH, "provider-only", "provider-only|any", "Whether an emailed code may sign into a person a provider knows (AUTH.md §8.4).", oidc.email_link_any),
    { "OPENCHIME_OIDC_CONNECT_%d", CFG_CUSTOM, CFG_OPERATOR, G_AUTH, NULL,
      "A direct connection to your own provider, 1 to 4 (AUTH.md §8.5): `label=…;issuer=https://…;client_id=…;secret_file=…;subject=sub|oid;redirect=127.0.0.1|localhost`.",
      0, 0, NULL, 0, cfg_parse_connect, CFG_PATTERN, OC_OIDC_MAX_CONNECT },

    /* --- Outbound TLS ---------------------------------------------------------- */
    PATHR("OPENCHIME_EXTRA_CA", CFG_OPERATOR, G_TLS, NULL, "PEM roots trusted beside the built-in ones for every outbound connection (push, enrollment, unfurls, S3): a self-hosted service behind a private CA.", F(extra_ca), NULL, CFG_MUST_EXIST, 0),

    /* --- Certificates ---------------------------------------------------------- */
    ENUM("OPENCHIME_TLS_SOURCE", CFG_OPERATOR, G_CERT, NULL, "self|file|acme|central", "Where the daemon's certificate comes from (TLS.md): its own self-signed one, your files, ACME for OPENCHIME_TLS_NAME, or central. Unset: `central` on an enrolled managed box, else `self`.", tls_src.source),
    STR("OPENCHIME_TLS_NAME", CFG_OPERATOR, G_CERT, NULL, "The DNS names to certify, comma-separated; required by `acme`.", F(tls_src.names), NULL, 0, 0),
    STR("OPENCHIME_ACME_DIRECTORY", CFG_OPERATOR, G_CERT, "https://acme-v02.api.letsencrypt.org/directory", "The ACME directory URL.", F(tls_src.directory), NULL, 0, 0),
    STR("OPENCHIME_ACME_EMAIL", CFG_OPERATOR, G_CERT, NULL, "The ACME account's contact address.", F(tls_src.email), NULL, 0, 0),

    /* --- Attachments and blob storage ------------------------------------------ */
    PATHR("OPENCHIME_BLOB_DIR", CFG_OPERATOR, G_BLOB, "/data/blobs", "Where attachments are kept on local disk, and the filesystem whose free space the pressure policy measures. The packages set `/var/lib/openchime/blobs`.", F(blob_dir), NULL, 0, 0),
    U64("OPENCHIME_MAX_ATTACHMENT_SIZE", CFG_OPERATOR, G_BLOB, "209715200", 1, (int64_t)OC_MAX_ATTACHMENT_SIZE, "The largest attachment, in bytes; the wire's cap (200 MiB) is the most it can be.", max_attach_size),
    U64("OPENCHIME_MAX_VIDEO_MESSAGE_SIZE", CFG_OPERATOR, G_BLOB, "167772160", 1, (int64_t)OC_MAX_ATTACHMENT_SIZE, "The largest video message, in bytes; at most the attachment size.", max_video_size),
    STR("OPENCHIME_S3_ENDPOINT", CFG_OPERATOR, G_BLOB, NULL, "S3 instead of local disk (ARCH-70): the endpoint (`host`, `host:port`, or a URL whose scheme picks the transport). All four S3 values, or none.", F(s3.endpoint), NULL, 0, 0),
    STR("OPENCHIME_S3_BUCKET", CFG_OPERATOR, G_BLOB, NULL, "The bucket.", F(s3.bucket), NULL, 0, 0),
    STR("OPENCHIME_S3_ACCESS_KEY", CFG_OPERATOR, G_BLOB, NULL, "The access key.", F(s3.access_key), NULL, CFG_SECRET, 0),
    STR("OPENCHIME_S3_SECRET_KEY", CFG_OPERATOR, G_BLOB, NULL, "The secret key.", F(s3.secret_key), NULL, CFG_SECRET, 0),
    STR("OPENCHIME_S3_REGION", CFG_OPERATOR, G_BLOB, "us-east-1", "The region SigV4 signs for.", F(s3.region), NULL, 0, 0),

    /* --- Storage pressure and maintenance -------------------------------------- */
    U64("OPENCHIME_ATTACH_MAX_AGE_DAYS", CFG_OPERATOR, G_STOR, "0", 0, 36500, "Attachments older than this expire; 0 keeps them forever.", storage_raw.attach_max_age_days),
    BOOL("OPENCHIME_EVICT", CFG_OPERATOR, G_STOR, "on", "Whether the oldest attachments are evicted when free space falls below OPENCHIME_PRESSURE_MB (REQ-215). Local disk only.", storage_raw.evict),
    U64("OPENCHIME_EVICT_GRACE_HOURS", CFG_TUNING, G_STOR, "24", 0, 8760, "Never evict anything younger than this.", storage_raw.evict_grace_hours),
    U64("OPENCHIME_DB_RESERVE_MB", CFG_TUNING, G_STOR, "256", 1, 1048576, "Free space that belongs to the database: below it, uploads are refused (REQ-216). Measured on the attachment filesystem, so it protects the database only when the two share a disk.", storage_raw.db_reserve_mb),
    U64("OPENCHIME_PRESSURE_MB", CFG_TUNING, G_STOR, "512", 1, 1048576, "Free space below which eviction starts; at least the reserve.", storage_raw.pressure_mb),
    U64("OPENCHIME_MAINT_BATCH", CFG_TUNING, G_STOR, "64", 1, 100000, "The most attachments one maintenance pass reclaims.", storage_raw.maint_batch),
    U64("OPENCHIME_AUDIT_MAX_DAYS", CFG_OPERATOR, G_STOR, "365", 0, 36500, "Audit entries older than this age out, per family (REQ-251); 0 keeps them forever.", storage_raw.audit_max_days),
    U64("OPENCHIME_MAINT_INTERVAL_MS", CFG_DEMO, G_STOR, "300000", 1000, 86400000, "How often the maintenance pass runs; five minutes in a production build.", storage_raw.maint_interval_ms),
    U64("OPENCHIME_SCHED_TICK_MS", CFG_DEMO, G_STOR, "15000", 50, 3600000, "How often scheduled messages are swept; fifteen seconds in a production build.", sched_tick_ms),

    /* --- Federated services ---------------------------------------------------- */
    STR("OPENCHIME_ENROLL_URL", CFG_OPERATOR, G_FED, NULL, "The control plane to enrol with (ARCH-84): https, or http to loopback for a test. Prints an `oce1.` code to the log on first run.", F(enroll.url), NULL, 0, 0),
    PATHR("OPENCHIME_ENROLL_CODE_FILE", CFG_OPERATOR, G_FED, NULL, "Where the enrollment code is also written, for an installer to read.", F(enroll.code_file), NULL, 0, 0),
    INT("OPENCHIME_ENROLL_WAIT_SECS", CFG_OPERATOR, G_FED, "0", 0, 86400, "How long the first start waits for the enrollment to be claimed before serving anyway.", enroll.wait_secs),
    STR("OPENCHIME_ENROLL_TICKET", CFG_OPERATOR, G_FED, NULL, "A managed box's claim ticket, written by the control plane at provision time.", F(enroll.ticket), NULL, CFG_SECRET, 0),
    STR("OPENCHIME_PUSH_URL", CFG_OPERATOR, G_FED, NULL, "The mobile push relay (ARCH-16/85), contentless: https, or http to loopback for a test.", F(push.url), NULL, 0, 0),
    ENUM("OPENCHIME_INVITE_MAIL", CFG_OPERATOR, G_FED, "off", "off|on", "Whether invitations bound to an address are reported to central for it to mail (REQ-280); needs an active enrollment.", invite_mail),

    /* --- Audio ----------------------------------------------------------------- */
    INT("OPENCHIME_AUDIO_PORT", CFG_OPERATOR, G_AUDIO, "0", 0, 65535, "The call relay's UDP port; 0 is ephemeral (AUDIO.md §4).", audio_port),
    INT("OPENCHIME_AUDIO_ADVERTISE_PORT", CFG_OPERATOR, G_AUDIO, "0", 0, 65535, "The UDP port clients are told, where a front door maps it; 0 is the bound one.", audio_advertise_port),
    { "OPENCHIME_AUDIO_TOKEN_PREFIX", CFG_CUSTOM, CFG_OPERATOR, G_AUDIO, NULL,
      "Hex bytes (at most 16) that lead every relay token, so a front door can route by them.",
      0, 0, NULL, 0, cfg_parse_token_prefix, 0, 0 },
    INT("OPENCHIME_CALL_MAX", CFG_OPERATOR, G_CALLS, "10", 2, (int64_t)OC_MAX_CALL_PARTICIPANTS, "The most people in one call (REQ-305); every participant mixes every other's stream.", call_max),

    /* --- Read-aloud ------------------------------------------------------------ */
    BOOL("OPENCHIME_TTS", CFG_OPERATOR, G_TTS, "on", "Read-aloud (ARCH-111), in a build that carries it.", tts.enabled),
    STR("OPENCHIME_TTS_LANG", CFG_OPERATOR, G_TTS, "en-US", "The language read in; the binary carries one and refuses another.", F(tts.lang), NULL, 0, 0),
    PATHR("OPENCHIME_TTS_DATA_DIR", CFG_OPERATOR, G_TTS, NULL, "Where the voice data is; unset, `/usr/share/openchime/tts`, else `tts/` beside the executable.", F(tts.data_dir), NULL, 0, 0),
    INT("OPENCHIME_TTS_QUEUE", CFG_TUNING, G_TTS, "256", 1, 4096, "How many renders may wait.", tts.queue),
    INT("OPENCHIME_TTS_IDLE_SECS", CFG_TUNING, G_TTS, "300", 5, 86400, "How long the model stays loaded with nothing to do.", tts.idle_secs),
    INT("OPENCHIME_TTS_RATE", CFG_TUNING, G_TTS, "60", 1, 6000, "AUDIO_GETs one connection may make a minute.", tts.rate),

    /* --- Voice input ----------------------------------------------------------- */
    BOOL("OPENCHIME_STT", CFG_OPERATOR, G_STT, "on", "Voice input (ARCH-112), in a build that carries it.", stt.enabled),
    PATHR("OPENCHIME_STT_DATA_DIR", CFG_OPERATOR, G_STT, NULL, "Where the recognizer's data is; unset, `/usr/share/openchime/stt`, else `stt/` beside the executable.", F(stt.data_dir), NULL, 0, 0),
    INT("OPENCHIME_STT_QUEUE", CFG_TUNING, G_STT, "64", 1, 1024, "Segments that may wait for the recognizer.", stt.queue),
    INT("OPENCHIME_STT_IDLE_SECS", CFG_TUNING, G_STT, "300", 5, 86400, "How long the model stays loaded with nothing to hear.", stt.idle_secs),
    INT("OPENCHIME_STT_RATE", CFG_TUNING, G_STT, "60", 1, 6000, "Segments one connection may send a minute.", stt.rate),
    INT("OPENCHIME_STT_MAX_SECS", CFG_TUNING, G_STT, "30", 5, 60, "The longest segment.", stt.max_secs),

    /* --- Summaries ------------------------------------------------------------- */
    ENUM("OPENCHIME_SUMMARY", CFG_OPERATOR, G_SUM, "off", "off|local|cloud", "Summaries (REQ-310): `local` runs the model inside the daemon (fetched on first start), `cloud` names a hosted API.", summary.mode),
    STR("OPENCHIME_SUMMARY_URL", CFG_OPERATOR, G_SUM, "", "The hosted API's base URL, for `cloud`.", F(summary.url), NULL, 0, 0),
    STR("OPENCHIME_SUMMARY_MODEL", CFG_OPERATOR, G_SUM, "", "The model there, for `cloud`.", F(summary.model), NULL, 0, 0),
    STR("OPENCHIME_SUMMARY_API_KEY", CFG_OPERATOR, G_SUM, "", "Its key.", F(summary.api_key), NULL, CFG_SECRET, 0),

    /* --- Demo builds only -------------------------------------------------------- */
    BOOL("OPENCHIME_TEST_PASSWORD_AUTH", CFG_DEMO, G_DEMO, "off", "The daemon takes a local password in a frame, as the test suites and the harness sign in (AUTH.md §8.10).", demo.password_frames),
    BOOL("OPENCHIME_UNFURL_ALLOW_PRIVATE", CFG_DEMO, G_DEMO, "off", "Turns the unfurl fetcher's SSRF gate off, so a test can fetch a loopback fixture.", unfurl.allow_private),
    STR("OPENCHIME_BOOTSTRAP_USERS", CFG_DEMO, G_DEMO, NULL, "Accounts made at first start, `name:password:role,…`, for a fixture; a real deployment's first owner comes from the setup token.", F(bootstrap_users), NULL, CFG_SECRET, 0),
    INT("OPENCHIME_FILE_PAGE", CFG_DEMO, G_DEMO, "200", 1, (int64_t)OC_MAX_FILE_LIST, "How many files one LIST_FILES page carries; the wire's maximum in a production build.", file_page),
    PATHR("OPENCHIME_TEST_OS_ROOTS", CFG_DEMO, G_DEMO, NULL, "The CA bundle read instead of the operating system's, for a test with its own CA.", F(demo.os_roots), NULL, 0, 0),
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

    /* --- Client-side (every build) ---------------------------------------------- */
    CLIENT("OPENCHIME_SUFFIX", CFG_OPERATOR, G_CLIENT, "Clients: the DNS suffix appended to a bare workspace name (ARCH-14); `workspace.openchime.io` unset."),

    { NULL, CFG_STR, CFG_OPERATOR, NULL, NULL, NULL, 0, 0, NULL, 0, NULL, 0, 0 }
};

const cfg_removed OC_CFG_REMOVED[] = {
    { "OC_AUTH_MODE",         "renamed OPENCHIME_AUTH_MODE" },
    { "OC_OIDC_ISSUER",       "renamed OPENCHIME_OIDC_ISSUER" },
    { "OC_OIDC_AUDIENCE",     "renamed OPENCHIME_OIDC_AUDIENCE" },
    { "OC_OIDC_PUBKEY",       "renamed OPENCHIME_OIDC_PUBKEY" },
    { "OC_OIDC_PUBKEY_FILE",  "renamed OPENCHIME_OIDC_PUBKEY_FILE" },
    { "OC_BOOTSTRAP_USERS",   "renamed OPENCHIME_BOOTSTRAP_USERS (demo builds only)" },
    { "OC_ENROLL_URL",        "renamed OPENCHIME_ENROLL_URL" },
    { "OC_ENROLL_CODE_FILE",  "renamed OPENCHIME_ENROLL_CODE_FILE" },
    { "OC_ENROLL_WAIT_SECS",  "renamed OPENCHIME_ENROLL_WAIT_SECS" },
    { "OC_PUSH_URL",          "renamed OPENCHIME_PUSH_URL" },
    { "OC_TLS_OS_ROOTS",      "renamed OPENCHIME_TEST_OS_ROOTS (demo builds only)" },
    { "OPENCHIME_RECOVER_MB", "removed: nothing read it; eviction runs to OPENCHIME_PRESSURE_MB" },
    { "OPENCHIME_BLOB_BACKEND", "removed: the backend is S3 when the four OPENCHIME_S3_* values are set, local disk when none is" },
    { "OPENCHIME_STATE",      "removed: the TUI keeps its state in the credential store (ARCH-88)" },
    { NULL, NULL }
};
