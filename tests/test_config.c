/* The configuration loader (daemon/config.c) against the registry
 * (daemon/config_table.c): strict parsing, ranges, unknown and removed names,
 * the cross-row rules, the storage policy it derives, the mode and the development rows, the
 * EnvironmentFile, and the registry's own consistency (every default parses). */
#include "config.h"
#include "config_table.h"
#include "protocol.h"
#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Load with `name=value` set, into `err`. Returns the loader's status. */
static int load_with(const char *name, const char *value, char *err, size_t cap) {
    setenv(name, value, 1);
    err[0] = '\0';
    int rc = oc_config_load(err, cap);
    unsetenv(name);
    return rc;
}

static void refuses(const char *name, const char *value, const char *msg_has) {
    char err[512];
    int rc = load_with(name, value, err, sizeof err);
    CHECK(rc != 0);
    if (rc == 0) printf("    %s=%s was accepted\n", name, value);
    CHECK(strstr(err, name) != NULL);           /* the message names the variable */
    CHECK(strstr(err, msg_has) != NULL);
    if (!strstr(err, msg_has)) printf("    %s=%s: \"%s\" lacks \"%s\"\n", name, value, err, msg_has);
}

static void accepts(const char *name, const char *value) {
    char err[512];
    int rc = load_with(name, value, err, sizeof err);
    CHECK(rc == 0);
    if (rc != 0) printf("    %s=%s was refused: %s\n", name, value, err);
}

static void test_numbers(void) {
    refuses("OPENCHIME_PORT", "abc", "integer 1..65535");
    refuses("OPENCHIME_PORT", "12x", "integer 1..65535");
    refuses("OPENCHIME_PORT", " 8443", "integer 1..65535");
    refuses("OPENCHIME_PORT", "-1", "integer 1..65535");
    refuses("OPENCHIME_PORT", "70000", "integer 1..65535");
    refuses("OPENCHIME_PORT", "0", "integer 1..65535");
    accepts("OPENCHIME_PORT", "8443");
    accepts("OPENCHIME_PORT", "");                 /* empty is unset: the default */
    CHECK(oc_config_get()->proto_port == 8443);
    /* The health port: 0 is no listener, not an ephemeral one. */
    accepts("OPENCHIME_HEALTH_PORT", "0");
    CHECK(oc_config_get()->health_port == 0);
    refuses("OPENCHIME_HEALTH_PORT", "65536", "0..65535");
    refuses("OPENCHIME_DEV_CALL_MAX", "1", "2..32");
    refuses("OPENCHIME_DEV_FILE_PAGE", "0", "1..200");
}

static void test_bools_and_enums(void) {
    accepts("OPENCHIME_TTS", "0");       CHECK(oc_config_get()->tts.enabled == 0);
    accepts("OPENCHIME_TTS", "off");     CHECK(oc_config_get()->tts.enabled == 0);
    accepts("OPENCHIME_TTS", "false");   CHECK(oc_config_get()->tts.enabled == 0);
    accepts("OPENCHIME_STT", "yes");     CHECK(oc_config_get()->stt.enabled == 1);
    refuses("OPENCHIME_TTS", "2", "on, off");
    refuses("OPENCHIME_TTS", "maybe", "on, off");
    refuses("OPENCHIME_MFA", "Required", "off");   /* case matters */
    accepts("OPENCHIME_MFA", "required");
    CHECK(oc_config_get()->local_mfa == 2);
    accepts("OPENCHIME_MFA", "off");
    CHECK(oc_config_get()->local_mfa == 0);
    refuses("OPENCHIME_INVITE_MAIL", "yes", "off");       /* an enum, not a bool */
}

static void test_unknown_and_removed(void) {
    char err[512];
    int rc = load_with("OPENCHIME_PORTT", "1", err, sizeof err);
    CHECK(rc != 0);
    CHECK(strstr(err, "OPENCHIME_PORTT is not a setting") != NULL);
    CHECK(strstr(err, "OPENCHIME_PORT") != NULL);          /* the nearest name */
    rc = load_with("OPENCHIME_SOMETHING_ELSE", "1", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "is not a setting"));
    /* Every name that was a setting says what to do instead. */
    rc = load_with("OPENCHIME_DEPLOYMENT_MODE", "managed", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OPENCHIME_DEPLOYMENT_MODE is no longer read") && strstr(err, "OPENCHIME_MANAGED_TICKET"));
    rc = load_with("OPENCHIME_PROTO_PORT", "8443", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OPENCHIME_PORT"));
    rc = load_with("OPENCHIME_DB_PATH", "/x/db", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OPENCHIME_DATA_DIR"));
    rc = load_with("OPENCHIME_AUTH_MODE", "local", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OPENCHIME_AUTH"));
    rc = load_with("OPENCHIME_OIDC_ISSUER", "https://x", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OPENCHIME_ENROLL_URL"));
    rc = load_with("OPENCHIME_S3_ENDPOINT", "s3.example", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OPENCHIME_S3="));
    rc = load_with("OPENCHIME_SESSION_DAYS", "7", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "constant"));
    rc = load_with("OPENCHIME_TEST_PASSWORD_AUTH", "1", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "sign-in page"));
    rc = load_with("OPENCHIME_DEMO_PASSWORD_FRAMES", "on", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "sign-in page"));
    rc = load_with("OPENCHIME_RECOVER_MB", "5", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "no longer read"));
    /* An empty unknown name is not set, so it is not an error. */
    accepts("OPENCHIME_PORTT", "");
}

static void test_structured(void) {
    /* OPENCHIME_TLS. */
    refuses("OPENCHIME_TLS", "acme", "acme:<names>");
    refuses("OPENCHIME_TLS", "acme:", "needs the names");
    refuses("OPENCHIME_TLS", "letsencrypt", "self, file, central");
    refuses("OPENCHIME_TLS", "acme:a;colour=blue", "unknown key");
    refuses("OPENCHIME_TLS", "acme:a;email", "not key=value");
    refuses("OPENCHIME_TLS", "acme:a;directory=http://ca", "https");
    refuses("OPENCHIME_TLS", "acme:a b", "DNS names");
    refuses("OPENCHIME_TLS", "file", "cert.pem");    /* /data/cert.pem is not here */
    refuses("OPENCHIME_TLS", "central", "OPENCHIME_ENROLL_URL");
    accepts("OPENCHIME_TLS", "acme:chat.example.com,www.example.com;email=ops@example.com;directory=https://ca.internal/dir");
    CHECK(oc_config_get()->tls_src.source == OC_TLS_SRC_ACME);
    CHECK(!strcmp(oc_config_get()->tls_src.names, "chat.example.com,www.example.com"));
    CHECK(!strcmp(oc_config_get()->tls_src.email, "ops@example.com"));
    CHECK(!strcmp(oc_config_get()->tls_src.directory, "https://ca.internal/dir"));
    accepts("OPENCHIME_TLS", "acme:chat.example.com");
    CHECK(oc_config_get()->tls_src.email == NULL);
    CHECK(strstr(oc_config_get()->tls_src.directory, "letsencrypt") != NULL);
    accepts("OPENCHIME_TLS", "self");
    CHECK(oc_config_get()->tls_src.source == OC_TLS_SRC_SELF && oc_config_get()->tls_src.names == NULL);

    /* OPENCHIME_S3: one URL, everything in it; the message never echoes it. */
    char err[512];
    CHECK(load_with("OPENCHIME_S3", "s3://minio.local/bucket", err, sizeof err) != 0);
    CHECK(strstr(err, "KEY:SECRET") && !strstr(err, "minio.local"));
    refuses("OPENCHIME_S3", "https://k:s@h/b", "s3://");
    refuses("OPENCHIME_S3", "s3://k:s@h", "s3://");                 /* no bucket */
    refuses("OPENCHIME_S3", "s3://k:s@h/", "bucket");
    refuses("OPENCHIME_S3", "s3://k:s@h/b/c", "one path segment");
    refuses("OPENCHIME_S3", "s3://k:s@/b", "host");
    refuses("OPENCHIME_S3", "s3://k:@h/b", "neither empty");
    refuses("OPENCHIME_S3", "s3://k:s@h/b?colour=blue", "region");
    refuses("OPENCHIME_S3", "s3://k%zz:s@h/b", "percent-decode");
    accepts("OPENCHIME_S3", "s3://AKID:se%2Fcret%2Bx@minio.local:9000/bucket?region=eu-west-1");
    CHECK(!strcmp(oc_config_get()->s3.endpoint, "https://minio.local:9000"));
    CHECK(!strcmp(oc_config_get()->s3.bucket, "bucket"));
    CHECK(!strcmp(oc_config_get()->s3.access_key, "AKID"));
    CHECK(!strcmp(oc_config_get()->s3.secret_key, "se/cret+x"));
    CHECK(!strcmp(oc_config_get()->s3.region, "eu-west-1"));
    accepts("OPENCHIME_S3", "http+s3://k:s@127.0.0.1:9000/b");
    CHECK(!strcmp(oc_config_get()->s3.endpoint, "http://127.0.0.1:9000"));
    CHECK(!strcmp(oc_config_get()->s3.region, "us-east-1"));       /* the default */
    accepts("OPENCHIME_S3", "");
    CHECK(oc_config_get()->s3.endpoint == NULL);                   /* local disk */

    /* OPENCHIME_SUMMARY. */
    refuses("OPENCHIME_SUMMARY", "cloud", "cloud:<url>,<model>");
    refuses("OPENCHIME_SUMMARY", "cloud:https://api.example", "<url>,<model>");
    refuses("OPENCHIME_SUMMARY", "cloud:api.example,m", "http");
    refuses("OPENCHIME_SUMMARY", "cloud:https://api.example,", "<url>,<model>");
    refuses("OPENCHIME_SUMMARY", "on", "off, local");
    accepts("OPENCHIME_SUMMARY", "cloud:https://api.example/v1,modelname");
    CHECK(oc_config_get()->summary.mode == OC_SUMMARY_CLOUD);
    CHECK(!strcmp(oc_config_get()->summary.url, "https://api.example/v1"));
    CHECK(!strcmp(oc_config_get()->summary.model, "modelname"));
    accepts("OPENCHIME_SUMMARY", "local");
    CHECK(oc_config_get()->summary.mode == OC_SUMMARY_LOCAL);
    accepts("OPENCHIME_SUMMARY", "");
    CHECK(oc_config_get()->summary.mode == OC_SUMMARY_OFF);

    /* OPENCHIME_AUTH. */
    refuses("OPENCHIME_AUTH", "oidc", "unknown source");
    accepts("OPENCHIME_AUTH", "");                                 /* empty is the default: local */
    CHECK(oc_config_get()->auth_local == 1 && oc_config_get()->auth_relay == 0);
    refuses("OPENCHIME_AUTH", ",", "names no source");
    refuses("OPENCHIME_AUTH", "relay", "OPENCHIME_ENROLL_URL");
    setenv("OPENCHIME_ENROLL_URL", "http://127.0.0.1:9/cp", 1);
    accepts("OPENCHIME_AUTH", "local, relay");
    CHECK(oc_config_get()->auth_local == 1 && oc_config_get()->auth_relay == 1);
    CHECK(!strcmp(oc_config_get()->oidc.pubkey, ""));              /* no pinned key: the published ones */
    unsetenv("OPENCHIME_ENROLL_URL");
}

static void test_cross_rules(void) {
    char err[512];
    /* Transport: signed requests go over https, or http to loopback. */
    refuses("OPENCHIME_ENROLL_URL", "http://openchime.io", "https");
    accepts("OPENCHIME_ENROLL_URL", "http://127.0.0.1:9000");
    CHECK(oc_config_get()->deployment_mode == OC_DEPLOY_FEDERATED);    /* derived */
    refuses("OPENCHIME_PUSH_URL", "ftp://x", "https");
    /* A missing file is refused at load, not at first use. */
    refuses("OPENCHIME_EXTRA_CA", "/no/such/file.pem", "readable file");
    /* Direct connections: 1..4, each complete. */
    refuses("OPENCHIME_OIDC_CONNECT_5", "label=x", "not a setting");
    refuses("OPENCHIME_OIDC_CONNECT_2", "label=x", "needs label, an https issuer and client_id");
    refuses("OPENCHIME_OIDC_CONNECT_1", "label=x;issuer=https://i;client_id=c;colour=blue", "unknown key");
    accepts("OPENCHIME_OIDC_CONNECT_2", "label=Work;issuer=https://login.example;client_id=abc;subject=oid");
    CHECK(oc_config_get()->n_connect == 1 && oc_config_get()->connect[0].subject_oid == 1);
    /* The trusted proxies parse. */
    refuses("OPENCHIME_TRUSTED_PROXIES", "not an address", "OPENCHIME_TRUSTED_PROXIES");
    accepts("OPENCHIME_TRUSTED_PROXIES", "127.0.0.0/8");

    /* The managed rows (the tests run in development mode, which takes them
     * without the ticket: the fixtures stand in for a managed box). */
    refuses("OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX", "abc", "even number of hex");
    refuses("OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX", "zz", "hex");
    accepts("OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX", "0a0b");
    CHECK(oc_config_get()->audio_token_prefix_len == 2 && oc_config_get()->audio_token_prefix[1] == 0x0b);
    accepts("OPENCHIME_MANAGED_MAX_USERS", "5");
    CHECK(oc_config_get()->max_users == 5);
    CHECK(oc_config_get()->deployment_mode == OC_DEPLOY_STANDALONE);    /* a row is not a ticket */
    /* The ticket comes with the rest of a managed start. */
    refuses("OPENCHIME_MANAGED_TICKET", "t", "OPENCHIME_ENROLL_URL");
    setenv("OPENCHIME_ENROLL_URL", "http://127.0.0.1:9000", 1);
    refuses("OPENCHIME_MANAGED_TICKET", "t", "OPENCHIME_MANAGED_AUDIENCE");
    setenv("OPENCHIME_MANAGED_AUDIENCE", "ws_x", 1);
    accepts("OPENCHIME_MANAGED_TICKET", "t");
    setenv("OPENCHIME_MANAGED_TICKET", "t", 1);
    CHECK(oc_config_load(err, sizeof err) == 0);
    CHECK(oc_config_get()->deployment_mode == OC_DEPLOY_MANAGED);
    CHECK(oc_config_get()->tls_src.source == OC_TLS_SRC_CENTRAL);      /* the managed default */
    CHECK(!strcmp(oc_config_get()->oidc.audience, "ws_x"));
    setenv("OPENCHIME_TLS", "self", 1);
    CHECK(oc_config_load(err, sizeof err) == 0 && oc_config_get()->tls_src.source == OC_TLS_SRC_SELF);
    unsetenv("OPENCHIME_TLS");
    accepts("OPENCHIME_TLS", "central");                              /* with an enrollment: fine */
    unsetenv("OPENCHIME_MANAGED_TICKET"); unsetenv("OPENCHIME_MANAGED_AUDIENCE"); unsetenv("OPENCHIME_ENROLL_URL");
}

static void test_data_dir(void) {
    accepts("OPENCHIME_DATA_DIR", "/tmp/x/");
    const oc_config *c = oc_config_get();
    CHECK(!strcmp(c->db_path, "/tmp/x/openchime.db"));
    CHECK(!strcmp(c->blob_dir, "/tmp/x/blobs"));
    CHECK(!strcmp(c->tls_cert, "/tmp/x/cert.pem"));
    CHECK(!strcmp(c->tls_key, "/tmp/x/key.pem"));
    CHECK(!strcmp(c->factor_key_file, "/tmp/x/factor.key"));
    accepts("OPENCHIME_DATA_DIR", "");
    CHECK(!strcmp(oc_config_get()->db_path, "/data/openchime.db"));
}

static void test_constants(void) {
    /* The policy is the design's numbers, not rows. */
    accepts("OPENCHIME_WORKSPACE_NAME", "x");
    const oc_config *c = oc_config_get();
    CHECK(c->storage.interval_ms == 5 * 60 * 1000);
    CHECK(c->storage.max_age_ms == 0);
    CHECK(c->storage.evict_enabled == 1);
    CHECK(c->storage.reserve_bytes == 256ull * 1024 * 1024);
    CHECK(c->storage.pressure_bytes == 512ull * 1024 * 1024);
    CHECK(c->storage.batch == 64);
    CHECK(c->storage.audit_max_age_ms == 365ull * 24 * 3600 * 1000);
    CHECK(c->sched_tick_ms == 15000);
    CHECK(c->call_max == OC_CALL_MAX && c->file_page == 200 && c->max_conns_per_ip == OC_MAX_CONNS_PER_IP);
    CHECK(OC_MAX_VIDEO_SIZE < OC_MAX_ATTACHMENT_SIZE);
}

/* Load with `mode` as OPENCHIME_MODE and `name=value` set. */
static int load_in_mode(const char *mode, const char *name, const char *value, char *err, size_t cap) {
    setenv("OPENCHIME_MODE", mode, 1);
    int rc = load_with(name, value, err, cap);
    setenv("OPENCHIME_MODE", "development", 1);
    return rc;
}

static void test_mode(void) {
    char err[512];
    /* The suites run in development mode: the development rows are read. */
    CHECK(oc_config_load(err, sizeof err) == 0 && oc_config_get()->mode == OC_MODE_DEVELOPMENT);
    accepts("OPENCHIME_DEV_FILE_PAGE", "2");
    CHECK(oc_config_get()->file_page == 2);
    accepts("OPENCHIME_DEV_CALL_MAX", "3");
    CHECK(oc_config_get()->call_max == 3);
    accepts("OPENCHIME_DEV_CONNS_PER_IP", "0");
    CHECK(oc_config_get()->max_conns_per_ip == 0);
    accepts("OPENCHIME_DEV_USERS", "alice:pw:owner");
    CHECK(!strcmp(oc_config_get()->dev.users, "alice:pw:owner"));
    /* The clients' inputs are known names, not stored. */
    accepts("OPENCHIME_DEV_AUDIO", "synthetic");
    accepts("OPENCHIME_DEV_DIR", "/tmp/x");
    accepts("OPENCHIME_DEV_SUFFIX", "chat.example");
    /* Development enrols with a loopback test service only. */
    refuses("OPENCHIME_ENROLL_URL", "https://openchime.io", "loopback");

    /* The values, and nothing else. */
    refuses("OPENCHIME_MODE", "production", "normal");
    refuses("OPENCHIME_MODE", "Development", "development");   /* case matters */
    /* normal and demo: every development row refuses the start, naming itself
     * and the mode; the constants hold, and the managed rows want the ticket. */
    static const char *const modes[] = { "normal", "demo" };
    static const char *const devs[][2] = {
        { "OPENCHIME_DEV_USERS", "alice:pw:owner" }, { "OPENCHIME_DEV_FILE_PAGE", "2" },
        { "OPENCHIME_DEV_CALL_MAX", "3" }, { "OPENCHIME_DEV_CONNS_PER_IP", "0" },
        { "OPENCHIME_DEV_AUDIO", "synthetic" }, { "OPENCHIME_DEV_SIGNIN", "alice:pw" },
    };
    /* A demo admits visitors with the control plane's invite to local accounts:
     * it needs both sources, and is refused without. */
    CHECK(load_in_mode("demo", "OPENCHIME_WORKSPACE_NAME", "x", err, sizeof err) != 0 && strstr(err, "local,relay"));
    for (size_t m = 0; m < sizeof modes / sizeof *modes; m++) {
        if (m == 1) { setenv("OPENCHIME_AUTH", "local,relay", 1); setenv("OPENCHIME_ENROLL_URL", "https://cp.example/api/machine/enroll", 1); }
        for (size_t i = 0; i < sizeof devs / sizeof *devs; i++) {
            int rc = load_in_mode(modes[m], devs[i][0], devs[i][1], err, sizeof err);
            CHECK(rc != 0 && strstr(err, devs[i][0]) && strstr(err, "development mode only") && strstr(err, modes[m]));
            if (rc == 0) printf("    %s=%s was accepted in %s mode\n", devs[i][0], devs[i][1], modes[m]);
        }
        CHECK(load_in_mode(modes[m], "OPENCHIME_WORKSPACE_NAME", "x", err, sizeof err) == 0);
        setenv("OPENCHIME_MODE", modes[m], 1);
        CHECK(oc_config_load(err, sizeof err) == 0);
        const oc_config *c = oc_config_get();
        CHECK(c->mode == (m == 0 ? OC_MODE_NORMAL : OC_MODE_DEMO));
        CHECK(c->call_max == OC_CALL_MAX && c->file_page == (int)OC_MAX_FILE_LIST && c->max_conns_per_ip == OC_MAX_CONNS_PER_IP);
        CHECK(c->dev.users == NULL);
        setenv("OPENCHIME_MODE", "development", 1);
        CHECK(load_in_mode(modes[m], "OPENCHIME_MANAGED_MAX_USERS", "5", err, sizeof err) != 0 && strstr(err, "OPENCHIME_MANAGED_TICKET"));
        if (m == 1) { unsetenv("OPENCHIME_AUTH"); unsetenv("OPENCHIME_ENROLL_URL"); }
    }
    /* Unset is normal. */
    unsetenv("OPENCHIME_MODE");
    CHECK(oc_config_load(err, sizeof err) == 0 && oc_config_get()->mode == OC_MODE_NORMAL);
    CHECK(load_with("OPENCHIME_DEV_USERS", "alice:pw:owner", err, sizeof err) != 0 && strstr(err, "OPENCHIME_MODE is normal"));
    setenv("OPENCHIME_MODE", "development", 1);

    /* The names a demo build read are gone, each naming what replaced it. */
    refuses("OPENCHIME_DEMO_USERS", "alice:pw:owner", "OPENCHIME_DEV_USERS");
    refuses("OPENCHIME_DEMO_CONNS_PER_IP", "0", "OPENCHIME_DEV_CONNS_PER_IP");
    refuses("OPENCHIME_DEMO_MAINT_MS", "1000", "five minutes");
    refuses("OPENCHIME_TEST_AUDIO", "synthetic", "OPENCHIME_DEV_AUDIO");
    CHECK(oc_config_load(err, sizeof err) == 0);
}

static void test_reload_and_registry(void) {
    /* Loading twice gives the same answer. */
    char err[512];
    CHECK(oc_config_load(err, sizeof err) == 0);
    char db1[1024]; snprintf(db1, sizeof db1, "%s", oc_config_get()->db_path);
    CHECK(oc_config_load(err, sizeof err) == 0);
    CHECK(!strcmp(db1, oc_config_get()->db_path));
    /* Every row: a name, a group, a doc line; a class; a default that its own
     * kind accepts (a default that fails its parser is a bug in the table). */
    int rows = 0, operator_rows = 0, managed_rows = 0, dev_rows = 0;
    for (const cfg_row *r = OC_CFG; r->name; r++, rows++) {
        CHECK(r->group && r->doc && r->doc[0]);
        CHECK(strncmp(r->name, "OPENCHIME_", 10) == 0);
        if (r->dflt && r->kind == CFG_INT) { long long v = atoll(r->dflt); CHECK(v >= r->min && v <= r->max); }
        if (r->dflt && r->kind == CFG_U64) { long long v = atoll(r->dflt); CHECK(v >= r->min && v <= r->max); }
        if (r->dflt && r->kind == CFG_BOOL) CHECK(!strcmp(r->dflt, "on") || !strcmp(r->dflt, "off"));
        if (r->dflt && r->kind == CFG_ENUM) CHECK(strstr(r->enums, r->dflt) != NULL);
        if (r->kind == CFG_CUSTOM) CHECK(r->parse != NULL);
        if (r->flags & CFG_PATTERN) CHECK(strstr(r->name, "%d") && r->count > 0);
        /* The classes are in the names. */
        if (r->cls == CFG_MANAGED) { managed_rows++; CHECK(strncmp(r->name, "OPENCHIME_MANAGED_", 18) == 0); }
        else if (r->cls == CFG_DEV) { dev_rows++; CHECK(!strncmp(r->name, "OPENCHIME_DEV_", 14)); }
        else { operator_rows++; CHECK(strncmp(r->name, "OPENCHIME_MANAGED_", 18) && strncmp(r->name, "OPENCHIME_DEV_", 14)); }
    }
    CHECK(operator_rows == 20);     /* OPENCHIME_MODE among them */
    CHECK(managed_rows == 7);
    CHECK(dev_rows == 4 + 12);      /* the daemon's four, and the clients' */
    for (const cfg_removed *x = OC_CFG_REMOVED; x->name; x++) CHECK(x->instead && x->instead[0]);
    /* No row is also a removed name. */
    for (const cfg_removed *x = OC_CFG_REMOVED; x->name; x++)
        for (const cfg_row *r = OC_CFG; r->name; r++) CHECK(strcmp(r->name, x->name) != 0);
}

static void test_env_file(void) {
    char err[512];
    const char *path = "build/oc-config-test.env";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f) return;
    fputs("# a comment\n\n"
          "OPENCHIME_WORKSPACE_NAME=\"Acme Inc\"\n"
          "  OPENCHIME_MANAGED_MAX_USERS='12'  \n"
          "export OPENCHIME_HEALTH_PORT=0\n", f);
    fclose(f);
    unsetenv("OPENCHIME_WORKSPACE_NAME"); unsetenv("OPENCHIME_MANAGED_MAX_USERS"); unsetenv("OPENCHIME_HEALTH_PORT");
    CHECK(oc_config_apply_env_file(path, err, sizeof err) == 0);
    CHECK(oc_config_load(err, sizeof err) == 0);
    CHECK(!strcmp(oc_config_get()->workspace_name, "Acme Inc"));   /* quotes stripped */
    CHECK(oc_config_get()->max_users == 12);
    CHECK(oc_config_get()->health_port == 0);
    unsetenv("OPENCHIME_WORKSPACE_NAME"); unsetenv("OPENCHIME_MANAGED_MAX_USERS"); unsetenv("OPENCHIME_HEALTH_PORT");
    /* A line that is not NAME=value names its line. */
    f = fopen(path, "w"); fputs("OPENCHIME_PORT=8443\nthis is not a setting\n", f); fclose(f);
    CHECK(oc_config_apply_env_file(path, err, sizeof err) != 0 && strstr(err, ":2:"));
    unsetenv("OPENCHIME_PORT");
    CHECK(oc_config_apply_env_file("/no/such/file", err, sizeof err) != 0);
    /* The packaged file is complete and loads. */
    CHECK(oc_config_apply_env_file("packaging/openchimed.env", err, sizeof err) == 0);
    CHECK(oc_config_load(err, sizeof err) == 0);
    if (oc_config_load(err, sizeof err) != 0) printf("    packaged env: %s\n", err);
    CHECK(!strcmp(oc_config_get()->db_path, "/var/lib/openchime/openchime.db"));
    for (const cfg_row *r = OC_CFG; r->name; r++) if (!(r->flags & CFG_PATTERN)) unsetenv(r->name);
    remove(path);
    /* Back to the suite's own configuration (tests/main.c). */
    CHECK(oc_config_load(err, sizeof err) == 0);
}

int run_config_tests(void) {
    printf("test_config: strict numbers, bools and enums; unknown and removed names; the structured values; "
           "the cross-row rules; the data directory; the constants; the mode; reload; the registry's "
           "own consistency; the EnvironmentFile\n");
    char err[512];
    CHECK(oc_config_load(err, sizeof err) == 0);
    test_numbers();
    test_bools_and_enums();
    test_unknown_and_removed();
    test_structured();
    test_cross_rules();
    test_data_dir();
    test_constants();
    test_mode();
    test_reload_and_registry();
    test_env_file();
    return failures;
}
