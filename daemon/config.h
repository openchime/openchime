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
 * SMALL. An operator sees settings and feature flags: what to decide, with a
 * value where there is anything to configure. The control plane's rows
 * (CFG_MANAGED) are refused unless the box is started with its ticket, and
 * the development rows (CFG_DEV) are refused unless OPENCHIME_MODE is
 * development. What nobody varies is a constant below, in code, not a row.
 *
 * STRICT. A value that does not parse, a value outside its range, a name the
 * registry does not know (a typo, a removed name) or a row that is not this
 * box's to set: each stops the daemon before it opens anything, with one line
 * naming the variable, the value and what is accepted. There is no soft mode;
 * `openchimed --check-config` is how an operator tests a file before a
 * restart. An EMPTY value is unset (the default applies), which is the one
 * leniency, and systemd's own convention.
 *
 * String fields point into the process environment, which the daemon never
 * mutates after loading, or into the loader's own buffers; either is valid for
 * the process lifetime.
 */

#ifndef OC_CONFIG_H
#define OC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#include "storage.h"    /* oc_storage_policy */

/* Deployment model (ARCH-76), derived: managed when started with the control
 * plane's ticket, federated when enrolled, else standalone. */
typedef enum {
    OC_DEPLOY_STANDALONE = 0,
    OC_DEPLOY_FEDERATED  = 1,
    OC_DEPLOY_MANAGED    = 2,
} oc_deploy_mode;

/* --- The constants that were once settings -----------------------------------
 * Each is here because nothing in any deployment varies it: the number is the
 * design, and a row would only be a way to get it wrong. */
#define OC_DEMO_SESSION_TTL_MS (24ull * 60 * 60 * 1000)   /* a demo's sessions: a day */
#define OC_SESSION_TTL_MS      (30ull * 24 * 60 * 60 * 1000)   /* a session lives 30 days (REQ-181) */
#define OC_SESSION_IDLE_MS     0ull                            /* and may go unused as long as it lives */
#define OC_XFER_WORKERS        2       /* threads moving attachment bytes (ARCH-69); each holds a TLS session */
/* How old an object in a managed box's store must be before the sweep may
 * delete it for want of a row naming it: past the replica's 14-day restore
 * window (entrypoint.sh) with room to spare, so no database a restore could
 * bring back still names it. */
#define OC_BLOB_SWEEP_AGE_MS   (30ull * 24 * 3600 * 1000)
#define OC_MAX_CONNS_PER_IP    1024    /* connections one address may hold: a large office behind one NAT */
#define OC_CALL_MAX            10      /* the most people in one call (REQ-305, CALLS.md) */
#define OC_TTS_QUEUE           256     /* renders that may wait */
#define OC_TTS_IDLE_SECS       300     /* the voice model stays loaded this long with nothing to do */
#define OC_TTS_GETS_PER_MIN    60      /* AUDIO_GETs one connection may make a minute */
#define OC_STT_QUEUE           64      /* segments that may wait for the recognizer */
#define OC_STT_IDLE_SECS       300
#define OC_STT_SEGS_PER_MIN    60      /* segments one connection may send a minute */
#define OC_STT_MAX_SECS        30      /* the longest segment: the model's own limit */
#define OC_CLAIM_WAIT_SECS     120     /* how long a managed box's first start waits for central */

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

/* What the box is for (OPENCHIME_MODE, CONFIG.md "Mode"): the one setting that
 * says a box is not an ordinary deployment. Every build is the same binary. */
typedef enum {
    OC_MODE_NORMAL      = 0,  /* a deployment: the constants, no development rows */
    OC_MODE_DEVELOPMENT = 1,  /* the tests and harnesses: the OPENCHIME_DEV_* rows are read */
    OC_MODE_DEMO        = 2,  /* the public demo workspace */
} oc_mode;

/* Where the certificate the daemon presents comes from (TLS.md, "Certificates"). */
typedef enum {
    OC_TLS_SRC_SELF    = 0,   /* its own self-signed certificate, made at first run */
    OC_TLS_SRC_FILE    = 1,   /* the operator's cert.pem and key.pem in the data directory */
    OC_TLS_SRC_ACME    = 2,   /* ACME (RFC 8555, TLS-ALPN-01) for the names given */
    OC_TLS_SRC_CENTRAL = 3,   /* through central, for a name under the service suffix */
} oc_tls_source;

/* The blob store's S3 backend (ARCH-70), from OPENCHIME_S3; endpoint NULL is
 * local disk. */
typedef struct {
    const char *endpoint, *bucket, *access_key, *secret_key, *region;
    const char *prefix;   /* keys go under <prefix>/ in the bucket; NULL: at its root */
} oc_s3_config;

typedef struct {
    /* Identity. */
    oc_mode        mode;                /* OPENCHIME_MODE */
    oc_deploy_mode deployment_mode;     /* derived (above) */
    const char *workspace_name;         /* "" ⇒ a client derives one from the host */
    const char *workspace_address;      /* OPENCHIME_MANAGED_ADDRESS; "" ⇒ clients keep what they dialled */

    /* OPENCHIME_DATA_DIR and its layout: everything the daemon keeps, under one
     * directory, at fixed names. */
    const char *data_dir;
    char db_path[1024], blob_dir[1024], tls_cert[1024], tls_key[1024], factor_key_file[1024];

    /* Ports. The call relay's UDP socket is on proto_port too (AUDIO.md §4). */
    int proto_port;
    int health_port;                    /* 0: no plaintext listener */

    /* The audio relay as clients reach it, where a front door maps it
     * (AUDIO.md §4; managed rows). */
    int     audio_advertise_port;       /* 0: the bound one */
    uint8_t audio_token_prefix[16];
    size_t  audio_token_prefix_len;

    int         max_users;              /* OPENCHIME_MANAGED_MAX_USERS; 0 = unlimited */
    const char *trusted_proxies;        /* peers whose PROXY v2 header is believed; NULL = none */
    const char *extra_ca;               /* PEM roots added to the built-in ones; NULL = none */

    oc_s3_config      s3;               /* endpoint NULL: local disk under data_dir/blobs */
    oc_storage_policy storage;          /* the maintenance pass's constants */
    uint64_t          sched_tick_ms;    /* the scheduled-send sweep: 15 s */

    /* Auth (OPENCHIME_AUTH): which sources are on. */
    int auth_local, auth_relay;
    int local_mfa;                      /* OPENCHIME_MFA: 0 off, 1 optional, 2 required */
    struct {
        const char *audience;           /* OPENCHIME_MANAGED_AUDIENCE: central minted it */
        const char *pubkey;             /* OPENCHIME_MANAGED_RELAY_KEY: a pinned floor under the published keys; "" none */
        const char *allow;              /* OPENCHIME_JOIN */
    } oidc;
    oc_oidc_connect connect[OC_OIDC_MAX_CONNECT];   /* direct connections, in order */
    int             n_connect;

    /* Federated services (ARCH-84/85). */
    struct { const char *url, *ticket; } enroll;
    struct { const char *url; } push;
    int invite_mail;                    /* REQ-280; needs an active enrollment */

    /* The certificate's source (OPENCHIME_TLS), and for ACME its names, CA and contact. */
    struct {
        oc_tls_source source;
        int           given;            /* set by the operator, not defaulted */
        const char   *names;            /* comma-separated DNS names, or NULL */
        const char   *directory;        /* the ACME directory URL */
        const char   *email;            /* ACME contact, or NULL */
    } tls_src;

    /* The speech features: built in and on by default (ARCH-111/112); their
     * data is found as every build's is (tts_data.h). */
    struct { int enabled; } tts, stt;
    /* Summaries (REQ-310, ARCH-116): OPENCHIME_SUMMARY is off, local or
     * cloud:<url>,<model>; OPENCHIME_SUMMARY_API_KEY is the hosted API's key. */
    struct { int mode; const char *url, *model, *api_key; } summary;
    uint64_t max_video_size;            /* OPENCHIME_MAX_VIDEO_SIZE: a video message's cap, sent to clients (REQ-164) */

    /* Development mode only (OPENCHIME_DEV_*). In any other mode these are the
     * constants and the variables refuse the start. */
    int call_max;                       /* OC_CALL_MAX unless OPENCHIME_DEV_CALL_MAX */
    int file_page;                      /* the wire's maximum unless OPENCHIME_DEV_FILE_PAGE */
    int max_conns_per_ip;               /* OC_MAX_CONNS_PER_IP unless OPENCHIME_DEV_CONNS_PER_IP */
    struct {
        const char *users;              /* OPENCHIME_DEV_USERS: accounts made at first start */
    } dev;
} oc_config;

/* Why a load stopped, as a kind and the names involved -- what a test or a
 * tool reads, where `err` is the operator's sentence and may be reworded. */
typedef enum {
    OC_CFGF_OK = 0,
    OC_CFGF_BAD_VALUE,      /* `name`'s value does not parse, is out of range, or is unusable */
    OC_CFGF_UNKNOWN,        /* `name` is no row; `other` the nearest row's name, or "" */
    OC_CFGF_REMOVED,        /* `name` was a setting and is no longer read */
    OC_CFGF_DEV_ONLY,       /* `name` is a development row and the mode is not development */
    OC_CFGF_MANAGED_ONLY,   /* `name` is the control plane's, and there is no ticket */
    OC_CFGF_NEEDS,          /* `name`'s value needs `other` set (or set otherwise) as well */
} oc_cfg_fault_kind;

typedef struct {
    oc_cfg_fault_kind kind;
    char name[64];         /* the variable at fault */
    char other[64];        /* the related variable, or "" */
} oc_cfg_fault;

/* The last oc_config_load's fault: kind OC_CFGF_OK after a load that succeeded. */
const oc_cfg_fault *oc_config_fault(void);

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

/* "normal" | "development" | "demo" -- for logs and --check-config. */
const char *oc_mode_name(oc_mode m);

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
