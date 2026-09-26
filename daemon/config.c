/* Unified daemon configuration loader (env → oc_config). See config.h. */

#include "config.h"
#include "proxyproto.h"
#include "protocol.h"   /* OC_MAX_ATTACHMENT_SIZE */
#include "tls.h"        /* oc_tls_set_extra_ca */

#include <ctype.h>
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
    if (c->audio_port < 0 || c->audio_port > 65535) {
        snprintf(err, errcap, "OPENCHIME_AUDIO_PORT=%d is not a port", c->audio_port);
        return -1;
    }
    /* Off unless set, so a daemon that is reached directly advertises exactly
     * what it bound, as it always has. A value that cannot be read stops the
     * boot: read wrongly, every call starts and nobody hears anything. */
    c->audio_advertise_port = env_int("OPENCHIME_AUDIO_ADVERTISE_PORT", NULL, 0);
    if (c->audio_advertise_port < 0 || c->audio_advertise_port > 65535) {
        snprintf(err, errcap, "OPENCHIME_AUDIO_ADVERTISE_PORT=%d is not a port",
                 c->audio_advertise_port);
        return -1;
    }
    {
        const char *hex = getenv("OPENCHIME_AUDIO_TOKEN_PREFIX");
        size_t n = hex ? strlen(hex) : 0;
        if (n % 2 || n / 2 > sizeof c->audio_token_prefix) {
            snprintf(err, errcap, "OPENCHIME_AUDIO_TOKEN_PREFIX must be an even number of hex "
                     "digits, at most %zu bytes", sizeof c->audio_token_prefix);
            return -1;
        }
        for (size_t i = 0; i < n; i += 2) {
            unsigned v;
            if (!isxdigit((unsigned char)hex[i]) || !isxdigit((unsigned char)hex[i + 1]) ||
                sscanf(hex + i, "%2x", &v) != 1) {
                snprintf(err, errcap, "OPENCHIME_AUDIO_TOKEN_PREFIX='%s' is not hex", hex);
                return -1;
            }
            c->audio_token_prefix[i / 2] = (uint8_t)v;
        }
        c->audio_token_prefix_len = n / 2;
    }

    /* Limits / tuning. */
    c->max_users       = env_int("OPENCHIME_MAX_USERS", NULL, 0);
    c->max_attach_size = env_u64("OPENCHIME_MAX_ATTACHMENT_SIZE", NULL, OC_MAX_ATTACHMENT_SIZE);
    if (c->max_attach_size == 0) c->max_attach_size = OC_MAX_ATTACHMENT_SIZE;
    /* A video message is an attachment first, so it can never be allowed more
     * than any attachment is: the upload would be refused before the cap was
     * consulted, and a larger setting would only be a number that lies. */
    c->max_video_size  = env_u64("OPENCHIME_MAX_VIDEO_MESSAGE_SIZE", NULL, OC_MAX_VIDEO_MESSAGE_SIZE);
    if (c->max_video_size == 0) c->max_video_size = OC_MAX_VIDEO_MESSAGE_SIZE;
    if (c->max_video_size > c->max_attach_size) c->max_video_size = c->max_attach_size;
    c->xfer_workers    = env_int("OPENCHIME_XFER_WORKERS", NULL, 2);
    if (c->xfer_workers < 1)  c->xfer_workers = 1;
    if (c->xfer_workers > 16) c->xfer_workers = 16;
    c->max_conns_per_ip = env_int("OPENCHIME_MAX_CONNS_PER_IP", NULL, 256);
    if (c->max_conns_per_ip < 0) c->max_conns_per_ip = 256;
    c->trusted_proxies = getenv("OPENCHIME_TRUSTED_PROXIES");
    {
        /* A list that cannot be read stops the boot: read wrongly it is either
         * "every client is one address" or "every connection is refused". */
        char why[160];
        oc_trusted_proxies *t = oc_trusted_proxies_parse(c->trusted_proxies, why, sizeof why);
        if (!t) {
            snprintf(err, errcap, "OPENCHIME_TRUSTED_PROXIES: %s", why);
            return -1;
        }
        oc_trusted_proxies_free(t);
    }
    /* Roots trusted beside the built-in ones, for a self-hosted service behind a
     * private CA. Read and checked now, so a file that is missing or that does
     * not parse stops the boot instead of failing each connection later. */
    c->extra_ca = getenv("OPENCHIME_EXTRA_CA");
    if (oc_tls_set_extra_ca(c->extra_ca) != 0) {
        snprintf(err, errcap, "OPENCHIME_EXTRA_CA='%s' is not a readable file of "
                              "PEM certificates", c->extra_ca);
        return -1;
    }
    c->blob_dir = env_or2("OPENCHIME_BLOB_DIR", NULL, "/data/blobs");

    /* Storage-pressure policy (reuse the storage-domain env parser + clamps). */
    oc_storage_policy_load(&c->storage);

    /* Auth. `oidc.audience` may be overridden at runtime by the daemon-generated
     * enrollment audience; the full OIDC required-field check stays in main.c
     * where that audience is resolved. */
    c->auth_mode      = env_or2("OPENCHIME_AUTH_MODE",     "OC_AUTH_MODE",     "local");
    c->oidc.issuer    = env_or2("OPENCHIME_OIDC_ISSUER",   "OC_OIDC_ISSUER",   NULL);
    c->oidc.audience  = env_or2("OPENCHIME_OIDC_AUDIENCE", "OC_OIDC_AUDIENCE", NULL);
    c->oidc.allow     = getenv("OPENCHIME_OIDC_ALLOW");
    const char *pk_file = env_or2("OPENCHIME_OIDC_PUBKEY_FILE", "OC_OIDC_PUBKEY_FILE", NULL);
    c->oidc.pubkey    = pk_file ? read_file(pk_file)
                                : env_or2("OPENCHIME_OIDC_PUBKEY", "OC_OIDC_PUBKEY", NULL);
    c->bootstrap_users = env_or2("OPENCHIME_BOOTSTRAP_USERS", "OC_BOOTSTRAP_USERS", NULL);

    /* Federated enrollment (CP-8). */
    c->enroll.url       = env_or2("OPENCHIME_ENROLL_URL",       "OC_ENROLL_URL",       NULL);
    c->enroll.code_file = env_or2("OPENCHIME_ENROLL_CODE_FILE", "OC_ENROLL_CODE_FILE", NULL);
    c->enroll.wait_secs = env_int("OPENCHIME_ENROLL_WAIT_SECS", "OC_ENROLL_WAIT_SECS", 0);
    c->enroll.ticket    = getenv("OPENCHIME_ENROLL_TICKET");

    /* Outbound push emitter (ARCH-85). */
    c->push.url = env_or2("OPENCHIME_PUSH_URL", "OC_PUSH_URL", NULL);

    /* Invitation mail (REQ-280's carve-out): whether each invite bound to an
     * address is reported to central for it to mail. Off unless asked for, and a
     * value that is neither word stops the boot rather than guessing which the
     * operator meant. */
    {
        const char *im = env_or2("OPENCHIME_INVITE_MAIL", NULL, "off");
        if      (strcmp(im, "on") == 0)  c->invite_mail = 1;
        else if (strcmp(im, "off") == 0) c->invite_mail = 0;
        else {
            snprintf(err, errcap, "OPENCHIME_INVITE_MAIL='%s' is invalid (want on|off)", im);
            return -1;
        }
    }

    /* Link unfurls (REQ-222, ARCH-105). Always on — no switch. An air-gapped
     * box needs none: its fetches simply fail, bounded and silent. */
    c->unfurl.allow_private = env_int("OPENCHIME_UNFURL_ALLOW_PRIVATE", NULL, 0);

    /* Read-aloud (ARCH-111). The model is in the binary, so the only reason to
     * turn this off is not wanting the feature or the memory a render takes. */
    c->tts.enabled   = env_int("OPENCHIME_TTS", NULL, 1) != 0;
    c->tts.queue     = env_int("OPENCHIME_TTS_QUEUE", NULL, 256);
    if (c->tts.queue < 1)    c->tts.queue = 1;
    if (c->tts.queue > 4096) c->tts.queue = 4096;
    /* Long enough that a listener working through a channel keeps the model
     * loaded, short enough that an idle tenant gives its memory back. */
    c->tts.idle_secs = env_int("OPENCHIME_TTS_IDLE_SECS", NULL, 300);
    if (c->tts.idle_secs < 5)     c->tts.idle_secs = 5;
    if (c->tts.idle_secs > 86400) c->tts.idle_secs = 86400;
    /* A listener plays messages one after another, so this bounds a client
     * asking for a whole history at once, not ordinary listening. */
    c->tts.rate      = env_int("OPENCHIME_TTS_RATE", NULL, 60);
    if (c->tts.rate < 1)    c->tts.rate = 1;
    if (c->tts.rate > 6000) c->tts.rate = 6000;
    /* Which language this deployment reads in. One is built into the binary, so
     * the only value it accepts is that one -- the knob exists so the language is
     * named rather than assumed, and so a daemon carrying two can be told which
     * to use without a new setting appearing from nowhere. An unknown value is
     * refused at startup by main.c rather than quietly falling back. */
    c->tts.lang      = env_or2("OPENCHIME_TTS_LANG", NULL, "en-US");

    /* Voice input (ARCH-112). Its language is the recognizer's own; there is no
     * second model to choose between, so it has no language setting. */
    c->stt.enabled   = env_int("OPENCHIME_STT", NULL, 1) != 0;
    c->stt.queue     = env_int("OPENCHIME_STT_QUEUE", NULL, 64);
    if (c->stt.queue < 1)    c->stt.queue = 1;
    if (c->stt.queue > 1024) c->stt.queue = 1024;
    c->stt.idle_secs = env_int("OPENCHIME_STT_IDLE_SECS", NULL, 300);
    if (c->stt.idle_secs < 5)     c->stt.idle_secs = 5;
    if (c->stt.idle_secs > 86400) c->stt.idle_secs = 86400;
    /* Free talk cuts at every pause, so a talker sends a segment every few
     * seconds; this bounds a client sending far faster than anyone speaks. */
    c->stt.rate      = env_int("OPENCHIME_STT_RATE", NULL, 60);
    if (c->stt.rate < 1)    c->stt.rate = 1;
    if (c->stt.rate > 6000) c->stt.rate = 6000;
    /* Moonshine recommends staying under about 30 seconds of input. */
    c->stt.max_secs  = env_int("OPENCHIME_STT_MAX_SECS", NULL, 30);
    if (c->stt.max_secs < 5)  c->stt.max_secs = 5;
    if (c->stt.max_secs > 60) c->stt.max_secs = 60;

    /* A call's size (REQ-305). Every participant receives every other's stream
     * and mixes them itself (AUDIO.md §1.1), so the cap bounds each client's
     * download and decoding, not only the relay's fan-out. */
    c->call_max = env_int("OPENCHIME_CALL_MAX", NULL, 10);
    if (c->call_max < 2) c->call_max = 2;
    if (c->call_max > (int)OC_MAX_CALL_PARTICIPANTS) c->call_max = (int)OC_MAX_CALL_PARTICIPANTS;
    c->file_page = env_int("OPENCHIME_FILE_PAGE", NULL, (int)OC_MAX_FILE_LIST);
    if (c->file_page < 1) c->file_page = 1;
    if (c->file_page > (int)OC_MAX_FILE_LIST) c->file_page = (int)OC_MAX_FILE_LIST;

    return 0;
}
