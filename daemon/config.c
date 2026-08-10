#error deliberate build break to verify required status checks
/* Unified daemon configuration loader (env → oc_config). See config.h. */

#include "config.h"
#include "protocol.h"   /* OC_MAX_ATTACHMENT_SIZE */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static oc_config g_cfg;

/* --- env helpers (the single copy; main.c/storage.c/blob_s3.c dupes removed) - */

/* Prefer the current `name`; fall back to a deprecated `alias` (logging once);
 * else `dflt`. Pass alias = NULL when a var has no legacy name. */
static const char *env_or2(const char *name, const char *alias, const char *dflt) {
    const char *v = getenv(name);
    if (v && *v) return v;
    if (alias) {
        v = getenv(alias);
        if (v && *v) {
            fprintf(stderr, "openchimed: %s is deprecated; use %s\n", alias, name);
            return v;
        }
    }
    return dflt;
}

static int env_int(const char *name, const char *alias, int dflt) {
    const char *v = env_or2(name, alias, NULL);
    return v ? atoi(v) : dflt;
}

static uint64_t env_u64(const char *name, const char *alias, uint64_t dflt) {
    const char *v = env_or2(name, alias, NULL);
    if (!v) return dflt;
    char *end = NULL;
    unsigned long long n = strtoull(v, &end, 10);
    return end == v ? dflt : (uint64_t)n;
}

/* Read a whole file into a malloc'd NUL-terminated buffer (OIDC pubkey), or NULL. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

const char *oc_deploy_mode_name(oc_deploy_mode m) {
    switch (m) {
        case OC_DEPLOY_FEDERATED: return "federated";
        case OC_DEPLOY_MANAGED:   return "managed";
        default:                  return "standalone";
    }
}

const oc_config *oc_config_get(void) { return &g_cfg; }

int oc_config_load(char *err, size_t errcap) {
    oc_config *c = &g_cfg;
    memset(c, 0, sizeof *c);

    /* Deployment model (infra). Unrecognized values are a hard error rather than
     * a silent default — an operator who typo'd the mode should be told. */
    const char *mode = env_or2("OPENCHIME_DEPLOYMENT_MODE", NULL, "standalone");
    if      (strcmp(mode, "standalone") == 0) c->deployment_mode = OC_DEPLOY_STANDALONE;
    else if (strcmp(mode, "federated")  == 0) c->deployment_mode = OC_DEPLOY_FEDERATED;
    else if (strcmp(mode, "managed")    == 0) c->deployment_mode = OC_DEPLOY_MANAGED;
    else {
        snprintf(err, errcap, "OPENCHIME_DEPLOYMENT_MODE='%s' is invalid "
                 "(want standalone|federated|managed)", mode);
        return -1;
    }
    c->workspace_name = env_or2("OPENCHIME_WORKSPACE_NAME", NULL, "");

    /* Paths + ports. */
    c->db_path     = env_or2("OPENCHIME_DB_PATH",   NULL, "/data/openchime.db");
    c->tls_cert    = env_or2("OPENCHIME_TLS_CERT",  NULL, "/data/cert.pem");
    c->tls_key     = env_or2("OPENCHIME_TLS_KEY",   NULL, "/data/key.pem");
    c->health_port = env_int("OPENCHIME_HEALTH_PORT", NULL, 8080);
    c->proto_port  = env_int("OPENCHIME_PROTO_PORT",  NULL, 8443);
    c->audio_port  = env_int("OPENCHIME_AUDIO_PORT",  NULL, 0);   /* 0 = ephemeral */

    /* Limits / tuning. */
    c->max_users       = env_int("OPENCHIME_MAX_USERS", NULL, 0);
    c->max_attach_size = env_u64("OPENCHIME_MAX_ATTACHMENT_SIZE", NULL, OC_MAX_ATTACHMENT_SIZE);
    if (c->max_attach_size == 0) c->max_attach_size = OC_MAX_ATTACHMENT_SIZE;
    c->xfer_workers    = env_int("OPENCHIME_XFER_WORKERS", NULL, 2);
    if (c->xfer_workers < 1)  c->xfer_workers = 1;
    if (c->xfer_workers > 16) c->xfer_workers = 16;
    c->max_conns_per_ip = env_int("OPENCHIME_MAX_CONNS_PER_IP", NULL, 256);
    if (c->max_conns_per_ip < 0) c->max_conns_per_ip = 256;
    c->blob_dir = env_or2("OPENCHIME_BLOB_DIR", NULL, "/data/blobs");

    /* Storage-pressure policy (reuse the storage-domain env parser + clamps). */
    oc_storage_policy_load(&c->storage);

    /* Auth. `oidc.audience` may be overridden at runtime by the daemon-generated
     * enrollment audience; the full OIDC required-field check stays in main.c
     * where that audience is resolved. */
    c->auth_mode      = env_or2("OPENCHIME_AUTH_MODE",     "OC_AUTH_MODE",     "local");
    c->oidc.issuer    = env_or2("OPENCHIME_OIDC_ISSUER",   "OC_OIDC_ISSUER",   NULL);
    c->oidc.audience  = env_or2("OPENCHIME_OIDC_AUDIENCE", "OC_OIDC_AUDIENCE", NULL);
    c->oidc.params    = env_or2("OPENCHIME_OIDC_PARAMS",   "OC_OIDC_PARAMS",   "");
    const char *pk_file = env_or2("OPENCHIME_OIDC_PUBKEY_FILE", "OC_OIDC_PUBKEY_FILE", NULL);
    c->oidc.pubkey    = pk_file ? read_file(pk_file)
                                : env_or2("OPENCHIME_OIDC_PUBKEY", "OC_OIDC_PUBKEY", NULL);
    c->bootstrap_users = env_or2("OPENCHIME_BOOTSTRAP_USERS", "OC_BOOTSTRAP_USERS", NULL);

    /* Federated enrollment (CP-8). */
    c->enroll.url       = env_or2("OPENCHIME_ENROLL_URL",       "OC_ENROLL_URL",       NULL);
    c->enroll.code_file = env_or2("OPENCHIME_ENROLL_CODE_FILE", "OC_ENROLL_CODE_FILE", NULL);
    c->enroll.ca_bundle = env_or2("OPENCHIME_ENROLL_CA_BUNDLE", "OC_ENROLL_CA_BUNDLE", NULL);
    c->enroll.wait_secs = env_int("OPENCHIME_ENROLL_WAIT_SECS", "OC_ENROLL_WAIT_SECS", 0);

    /* Outbound push emitter (ARCH-85). */
    c->push.url       = env_or2("OPENCHIME_PUSH_URL",       "OC_PUSH_URL",       NULL);
    c->push.ca_bundle = env_or2("OPENCHIME_PUSH_CA_BUNDLE", "OC_PUSH_CA_BUNDLE", NULL);

    return 0;
}
