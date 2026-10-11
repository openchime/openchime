/*
 * openchimed -- the daemon's configuration (ARCH-26): every environment
 * variable it reads, declared once in the registry (config_table.c), read
 * once at startup by oc_config_load() into one process-global oc_config that
 * everything reaches through oc_config_get(). Nothing else in the daemon
 * calls getenv (scripts/check/config.sh enforces it).
 *
 * The registry is the one source: the loader reads from it, `openchimed
 * --config-reference` prints CONFIG.md's reference from it, `--config-env`
 * prints the packaged environment file from it, and `--check-config` reports
 * the effective value and source of every row. A variable that is not a row
 * does not exist.
 *
 * STRICT. A value that does not parse, a value outside its range, a name the
 * registry does not know (a typo, a removed alias) or, in a production build,
 * a demo-only variable: each stops the daemon before it opens anything, with
 * one line naming the variable, the value and what is accepted. There is no
 * soft mode; `openchimed --check-config` is how an operator tests a file
 * before a restart. An EMPTY value is unset (the default applies), which is
 * the one leniency, and systemd's own convention.
 *
 * String fields point into the process environment, which the daemon never
 * mutates after loading, so they are valid for the process lifetime.
 */

#ifndef OC_CONFIG_H
#define OC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "storage.h"    /* oc_storage_policy */

/* Deployment model (ARCH-76). Set explicitly via OPENCHIME_DEPLOYMENT_MODE. */
typedef enum {
    OC_DEPLOY_STANDALONE = 0,
    OC_DEPLOY_FEDERATED  = 1,
    OC_DEPLOY_MANAGED    = 2,
} oc_deploy_mode;

/* A direct connection (AUTH.md §8.5), from OPENCHIME_OIDC_CONNECT_<n>:
 * `label=…;issuer=…;client_id=…;secret_file=…;subject=sub|oid;redirect=127.0.0.1|localhost`. */
#define OC_OIDC_MAX_CONNECT 4
typedef struct {
    char label[64];
    char issuer[256];
    char client_id[256];
    char secret[512];        /* "" for a public client: PKCE alone */
    int  subject_oid;        /* subject=oid: Microsoft's tenant-wide id, as the relay uses */
    int  localhost;          /* redirect=localhost: the provider matches that name */
} oc_oidc_connect;

enum { OC_SUMMARY_OFF = 0, OC_SUMMARY_LOCAL = 1, OC_SUMMARY_CLOUD = 2 };

/* Where the certificate the daemon presents comes from (TLS.md, "Certificates"). */
typedef enum {
    OC_TLS_SRC_SELF    = 0,   /* its own self-signed certificate, made at first run */
    OC_TLS_SRC_FILE    = 1,   /* the operator's, OPENCHIME_TLS_CERT / _KEY */
    OC_TLS_SRC_ACME    = 2,   /* ACME (RFC 8555, TLS-ALPN-01) for OPENCHIME_TLS_NAME */
    OC_TLS_SRC_CENTRAL = 3,   /* through central, for a name under the service suffix */
} oc_tls_source;

/* The blob store's backend configuration (ARCH-70): the four S3 values, all
 * set or none, chosen by configuration rather than a mode flag. */
typedef struct {
    const char *endpoint, *bucket, *access_key, *secret_key, *region;
} oc_s3_config;

typedef struct {
    /* Workspace identity (infra). */
    oc_deploy_mode deployment_mode;
    const char *workspace_name;     /* "" ⇒ client derives from the host subdomain */
    const char *workspace_address;  /* canonical address, "" ⇒ none: clients keep what they dialled */

    /* Paths + ports. */
    const char *db_path, *tls_cert, *tls_key;
    /* The key local accounts' second-step secrets are sealed under (AUTH.md
     * §8.6): OPENCHIME_FACTOR_KEY_FILE, else factor.key beside the database. */
    const char *factor_key_file_env;
    char        factor_key_file[1024];
    int         local_mfa;   /* OPENCHIME_LOCAL_MFA: 0 off, 1 optional, 2 required */
    /* How long a session lives, and may go unused (0: no limit), in days (REQ-181). */
    int         session_days, session_idle_days;
    int health_port, proto_port, audio_port;
    /* The audio relay as clients reach it, where that differs from the socket
     * (AUDIO.md §4): the UDP port CALL_JOINED names (0: the bound one), and the
     * bytes that lead every relay token so a front door can route by them. */
    int audio_advertise_port;
    uint8_t audio_token_prefix[16];
    size_t  audio_token_prefix_len;

    /* Limits / tuning. */
    int      max_users;             /* 0 = unlimited */
    int      xfer_workers;          /* 1..16 */
    int      max_conns_per_ip;      /* 0 disables */
    const char *trusted_proxies;    /* peers whose PROXY v2 header is believed; NULL = none */
    const char *extra_ca;           /* PEM roots added to the built-in ones; NULL = none */
    uint64_t max_attach_size;
    uint64_t max_video_size;        /* video message byte cap (REQ-164); <= max_attach_size */
    const char *blob_dir;
    oc_s3_config s3;                /* the S3 backend, when all four are set */

    /* Storage-pressure policy: the numbers as set (the table's rows) and the
     * policy derived from them in the units the maintenance pass uses. */
    struct {
        uint64_t maint_interval_ms, attach_max_age_days, evict_grace_hours,
                 db_reserve_mb, pressure_mb, maint_batch, audit_max_days;
        int      evict;
    } storage_raw;
    oc_storage_policy storage;
    /* The scheduled-send sweep (netloop.c), in ms; 15 s unless a demo build is
     * told otherwise. */
    uint64_t sched_tick_ms;

    /* Auth. */
    const char *auth_mode;          /* "local" | "oidc" | "relay", comma-separated */
    struct { const char *issuer, *audience, *pubkey, *pubkey_file, *allow;
             int email_link_any; } oidc;  /* OPENCHIME_OIDC_EMAIL_LINK=any */
    oc_oidc_connect connect[OC_OIDC_MAX_CONNECT];   /* direct connections, in order */
    int             n_connect;
    const char *bootstrap_users;    /* demo builds only */

    /* Federated enrollment (CP-8). */
    struct { const char *url, *code_file, *ticket; int wait_secs; } enroll;

    /* Outbound push emitter (ARCH-85). */
    struct { const char *url; } push;

    /* Invitation mail through central (REQ-280, ARCH-85): OPENCHIME_INVITE_MAIL,
     * off unless "on". Needs an active enrollment as well. */
    int invite_mail;

    /* The certificate's source, and for ACME its names, CA and contact. */
    struct {
        oc_tls_source source;
        const char   *source_env;   /* as given; NULL when the default applied */
        const char   *names;        /* comma-separated DNS names */
        const char   *directory;    /* the ACME directory URL */
        const char   *email;        /* ACME contact, or NULL */
    } tls_src;

    /* Link unfurls (REQ-222, ARCH-105). Always on -- there is no switch.
     * `allow_private` disables the SSRF gate; demo builds only. */
    struct { int allow_private; } unfurl;

    /* Read-aloud (REQ-291-295, ARCH-111). Built into the daemon and on by
     * default: there is nothing to install. `enabled` 0 turns it off, and a
     * daemon built without it has it off whatever this says. `queue` is how
     * many renders may wait, `idle_secs` how long the model stays loaded with
     * nothing to do, `rate` how many AUDIO_GETs a connection may make a minute. */
    struct { int enabled, queue, idle_secs, rate; const char *lang, *data_dir; } tts;
    /* Voice input (REQ-296-300, ARCH-112). Built in and on by default, with its
     * own data directory, so either speech feature can be missing without the
     * other. `queue` bounds segments waiting for the recognizer, `idle_secs` how
     * long the model stays loaded with nothing to hear, `rate` how many segments
     * a connection may send a minute, and `max_secs` the longest segment. */
    struct { int enabled, queue, idle_secs, rate, max_secs; const char *data_dir; } stt;
    /* Summaries of channels and DMs (REQ-310, ARCH-116): OPENCHIME_SUMMARY is
     * off, local or cloud; for cloud, OPENCHIME_SUMMARY_URL is the hosted API's
     * base, OPENCHIME_SUMMARY_MODEL names the model there and
     * OPENCHIME_SUMMARY_API_KEY is its key. */
    struct { int mode; const char *url, *model, *api_key; } summary;
    /* Calls (REQ-305): the most people in one call, 2 to OC_MAX_CALL_PARTICIPANTS. */
    int call_max;
    /* How many files one LIST_FILES page carries (REQ-143). The wire's maximum
     * unless a demo build is told otherwise, so a test can see a second page
     * without uploading two hundred and one files. */
    int file_page;

    /* Demo builds only (OC_DEMO): the test switches. In a production build
     * these fields are always zero and the variables refuse the start. */
    struct {
        int password_frames;        /* OPENCHIME_TEST_PASSWORD_AUTH: a password in a frame */
        const char *os_roots;       /* OPENCHIME_TEST_OS_ROOTS: the OS CA bundle to read instead */
    } demo;
} oc_config;

/* Load the daemon config from the environment into the process-global singleton.
 * Returns 0 on success; on any misconfiguration returns -1 and writes the
 * reason into `err` (up to `errcap`): the variable, the value and what is
 * accepted. Call once, early in main(), before any subsystem reads
 * oc_config_get(). Loading again replaces the previous load. */
int oc_config_load(char *err, size_t errcap);

/* The loaded config. Valid after a successful oc_config_load(); before that, all
 * fields are zero (deployment_mode = standalone, pointers NULL). */
const oc_config *oc_config_get(void);

/* "standalone" | "federated" | "managed" -- for logs and the wire. */
const char *oc_deploy_mode_name(oc_deploy_mode m);

/* Whether this binary is a demo build (OC_DEMO): the test switches exist and
 * `--version` says so. */
int oc_config_is_demo(void);

/* --- The command line's views of the registry (main.c) --------------------- */

/* Apply a systemd-style EnvironmentFile (NAME=value lines, `#` comments, single
 * or double quotes stripped, no expansion) to this process's environment, so a
 * following oc_config_load() reads it. Returns 0, or -1 with `err` set, which
 * names the line. Records which names the file set, for the report. */
int oc_config_apply_env_file(const char *path, char *err, size_t errcap);

/* Print every row's effective value and its source (default, environment, file)
 * to `out`, after a load; secrets are shown as `(set)`. */
void oc_config_report(void *out /* FILE* */);

/* CONFIG.md's reference section, as markdown, from the registry. */
void oc_config_print_reference(void *out /* FILE* */);

/* The packaged environment file, from the registry: every operator row,
 * commented, at its default. */
void oc_config_print_env_template(void *out /* FILE* */);

#endif /* OC_CONFIG_H */
