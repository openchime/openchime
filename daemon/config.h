/*
 * openchimed — unified daemon configuration (infra-level, env-sourced).
 *
 * Every environment variable that configures the *daemon* is read exactly once
 * at startup by oc_config_load() into a single process-global oc_config, reached
 * everywhere via oc_config_get(). This replaces the previous sprawl of getenv()
 * calls scattered across main.c / netloop.c / storage.c and three duplicated
 * getenv-or-default helpers.
 *
 * Config here is INFRA-LEVEL: authoritative in the environment, read once, never
 * runtime-mutable, never persisted to the DB (the DB is only for app-configurable
 * things like client_settings). Managed/federated deployments have these injected
 * as environment at provision time.
 *
 * NOTE: the pluggable blob storage *driver* (blobstore.c / blob_s3.c) reads its
 * own backend credentials (OPENCHIME_S3_* / OPENCHIME_BLOB_BACKEND) at open time
 * — that is driver config, like a database connection string, and stays in the
 * driver. Everything that configures the daemon itself lives here.
 *
 * String fields point into the process environment, which the daemon never
 * mutates (no setenv/putenv), so they are valid for the process lifetime; a few
 * resolved values (the OIDC pubkey read from a file) are malloc'd once and
 * intentionally never freed.
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

typedef struct {
    /* Workspace identity (infra). */
    oc_deploy_mode deployment_mode;
    const char *workspace_name;     /* "" ⇒ client derives from the host subdomain */

    /* Paths + ports. */
    const char *db_path, *tls_cert, *tls_key;
    int health_port, proto_port, audio_port;

    /* Limits / tuning. */
    int      max_users;             /* 0 = unlimited */
    int      xfer_workers;          /* clamped 1..16 */
    int      max_conns_per_ip;      /* 0 disables */
    uint64_t max_attach_size;
    const char *blob_dir;

    /* Storage-pressure policy (populated via oc_storage_policy_load). */
    oc_storage_policy storage;

    /* Auth. */
    const char *auth_mode;          /* "local" | "oidc" */
    struct { const char *issuer, *audience, *pubkey, *params; } oidc;  /* pubkey resolved */
    const char *bootstrap_users;

    /* Federated enrollment (CP-8). */
    struct { const char *url, *code_file, *ca_bundle; int wait_secs; } enroll;

    /* Outbound push emitter (ARCH-85). */
    struct { const char *url, *ca_bundle; } push;
} oc_config;

/* Load the daemon config from the environment into the process-global singleton.
 * Returns 0 on success; on a fatal misconfiguration returns -1 and writes a
 * human-readable reason into `err` (up to `errcap`). Call once, early in main(),
 * before any subsystem reads oc_config_get(). */
int oc_config_load(char *err, size_t errcap);

/* The loaded config. Valid after a successful oc_config_load(); before that, all
 * fields are zero (deployment_mode = standalone, pointers NULL). */
const oc_config *oc_config_get(void);

/* "standalone" | "federated" | "managed" — for logs and the wire. */
const char *oc_deploy_mode_name(oc_deploy_mode m);

#endif /* OC_CONFIG_H */
