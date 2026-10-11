/* The configuration loader (daemon/config.c) against the registry
 * (daemon/config_table.c): strict parsing, ranges, unknown and removed names,
 * the cross-row rules, the storage policy it derives, the demo switches, the
 * EnvironmentFile, and the registry's own consistency (every default parses). */
#include "config.h"
#include "config_table.h"
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
    refuses("OPENCHIME_PROTO_PORT", "abc", "integer 1..65535");
    refuses("OPENCHIME_PROTO_PORT", "12x", "integer 1..65535");
    refuses("OPENCHIME_PROTO_PORT", " 8443", "integer 1..65535");
    refuses("OPENCHIME_PROTO_PORT", "-1", "integer 1..65535");
    refuses("OPENCHIME_PROTO_PORT", "70000", "integer 1..65535");
    refuses("OPENCHIME_PROTO_PORT", "0", "integer 1..65535");
    accepts("OPENCHIME_PROTO_PORT", "8443");
    accepts("OPENCHIME_PROTO_PORT", "");                 /* empty is unset: the default */
    CHECK(oc_config_get()->proto_port == 8443);
    /* The silent clamps are ranges now. */
    refuses("OPENCHIME_XFER_WORKERS", "32", "1..16");
    refuses("OPENCHIME_XFER_WORKERS", "0", "1..16");
    accepts("OPENCHIME_XFER_WORKERS", "16");
    refuses("OPENCHIME_MAX_ATTACHMENT_SIZE", "200MB", "integer 1..");   /* no unit grammar */
    refuses("OPENCHIME_MAX_ATTACHMENT_SIZE", "0", "integer 1..");       /* "0 means default" is gone */
    accepts("OPENCHIME_MAX_ATTACHMENT_SIZE", "1048576");
    CHECK(oc_config_get()->max_attach_size == 1048576);
    CHECK(oc_config_get()->max_video_size == 1048576);   /* the video default follows a smaller cap down */
    refuses("OPENCHIME_CALL_MAX", "1", "2..32");
    refuses("OPENCHIME_TTS_IDLE_SECS", "4", "5..86400");
    refuses("OPENCHIME_SESSION_DAYS", "0", "1..3650");
    accepts("OPENCHIME_SESSION_IDLE_DAYS", "0");
}

static void test_bools_and_enums(void) {
    refuses("OPENCHIME_EVICT", "maybe", "on, off");
    accepts("OPENCHIME_EVICT", "off");   CHECK(oc_config_get()->storage.evict_enabled == 0);
    accepts("OPENCHIME_EVICT", "0");     CHECK(oc_config_get()->storage.evict_enabled == 0);
    accepts("OPENCHIME_EVICT", "false"); CHECK(oc_config_get()->storage.evict_enabled == 0);
    accepts("OPENCHIME_EVICT", "on");    CHECK(oc_config_get()->storage.evict_enabled == 1);
    accepts("OPENCHIME_EVICT", "yes");   CHECK(oc_config_get()->storage.evict_enabled == 1);
    accepts("OPENCHIME_TTS", "0");       CHECK(oc_config_get()->tts.enabled == 0);
    refuses("OPENCHIME_TTS", "2", "on, off");
    refuses("OPENCHIME_DEPLOYMENT_MODE", "hosted", "standalone");
    accepts("OPENCHIME_DEPLOYMENT_MODE", "federated");
    CHECK(oc_config_get()->deployment_mode == OC_DEPLOY_FEDERATED);
    refuses("OPENCHIME_LOCAL_MFA", "Required", "off");   /* case matters */
    accepts("OPENCHIME_LOCAL_MFA", "required");
    CHECK(oc_config_get()->local_mfa == 2);
    refuses("OPENCHIME_SUMMARY", "cloud", "OPENCHIME_SUMMARY_URL");
    refuses("OPENCHIME_INVITE_MAIL", "yes", "off");       /* an enum, not a bool */
    accepts("OPENCHIME_OIDC_EMAIL_LINK", "any");
    CHECK(oc_config_get()->oidc.email_link_any == 1);
}

static void test_unknown_and_removed(void) {
    char err[512];
    int rc = load_with("OPENCHIME_PROTOPORT", "1", err, sizeof err);
    CHECK(rc != 0);
    CHECK(strstr(err, "OPENCHIME_PROTOPORT is not a setting") != NULL);
    CHECK(strstr(err, "OPENCHIME_PROTO_PORT") != NULL);          /* the nearest name */
    rc = load_with("OPENCHIME_SOMETHING_ELSE", "1", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "is not a setting"));
    /* The removed aliases say what replaced them. */
    rc = load_with("OC_AUTH_MODE", "local", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OC_AUTH_MODE is no longer read") && strstr(err, "OPENCHIME_AUTH_MODE"));
    rc = load_with("OC_PUSH_URL", "https://x", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "OPENCHIME_PUSH_URL"));
    rc = load_with("OPENCHIME_RECOVER_MB", "5", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "no longer read"));
    rc = load_with("OPENCHIME_BLOB_BACKEND", "fs", err, sizeof err);
    CHECK(rc != 0 && strstr(err, "no longer read"));
    /* An empty unknown name is not set, so it is not an error. */
    accepts("OPENCHIME_PROTOPORT", "");
}

static void test_cross_rules(void) {
    char err[512];
    /* The S3 four: all or none. */
    refuses("OPENCHIME_S3_ENDPOINT", "s3.example", "set without OPENCHIME_S3_BUCKET");
    setenv("OPENCHIME_S3_ENDPOINT", "s3.example", 1);
    setenv("OPENCHIME_S3_BUCKET", "b", 1);
    setenv("OPENCHIME_S3_ACCESS_KEY", "a", 1);
    CHECK(oc_config_load(err, sizeof err) != 0 && strstr(err, "set without OPENCHIME_S3_SECRET_KEY"));
    setenv("OPENCHIME_S3_SECRET_KEY", "s", 1);
    CHECK(oc_config_load(err, sizeof err) == 0);
    CHECK(oc_config_get()->s3.endpoint && !strcmp(oc_config_get()->s3.bucket, "b"));
    CHECK(!strcmp(oc_config_get()->s3.region, "us-east-1"));       /* the default */
    unsetenv("OPENCHIME_S3_ENDPOINT"); unsetenv("OPENCHIME_S3_BUCKET");
    unsetenv("OPENCHIME_S3_ACCESS_KEY"); unsetenv("OPENCHIME_S3_SECRET_KEY");
    /* The watermarks: the operator's to order, not ours. */
    setenv("OPENCHIME_DB_RESERVE_MB", "100", 1);
    CHECK(load_with("OPENCHIME_PRESSURE_MB", "10", err, sizeof err) != 0 && strstr(err, "OPENCHIME_PRESSURE_MB=10"));
    CHECK(load_with("OPENCHIME_PRESSURE_MB", "100", err, sizeof err) == 0);
    unsetenv("OPENCHIME_DB_RESERVE_MB");
    /* A video message is an attachment first. */
    setenv("OPENCHIME_MAX_ATTACHMENT_SIZE", "1000000", 1);
    CHECK(load_with("OPENCHIME_MAX_VIDEO_MESSAGE_SIZE", "2000000", err, sizeof err) != 0 && strstr(err, "must not exceed"));
    unsetenv("OPENCHIME_MAX_ATTACHMENT_SIZE");
    /* The certificate's source needs what it needs. */
    refuses("OPENCHIME_TLS_SOURCE", "acme", "OPENCHIME_TLS_NAME");
    refuses("OPENCHIME_TLS_SOURCE", "file", "OPENCHIME_TLS_CERT");    /* /data/cert.pem is not here */
    refuses("OPENCHIME_TLS_SOURCE", "letsencrypt", "self");
    /* Transport: signed requests go over https, or http to loopback. */
    refuses("OPENCHIME_ENROLL_URL", "http://openchime.io", "https");
    accepts("OPENCHIME_ENROLL_URL", "http://127.0.0.1:9000");
    refuses("OPENCHIME_PUSH_URL", "ftp://x", "https");
    /* A missing file is refused at load, not at first use. */
    refuses("OPENCHIME_EXTRA_CA", "/no/such/file.pem", "readable file");
    refuses("OPENCHIME_OIDC_PUBKEY_FILE", "/no/such/key.pem", "readable file");
    /* Direct connections: 1..4, each complete. */
    refuses("OPENCHIME_OIDC_CONNECT_5", "label=x", "not a setting");
    refuses("OPENCHIME_OIDC_CONNECT_2", "label=x", "needs label, an https issuer and client_id");
    refuses("OPENCHIME_OIDC_CONNECT_1", "label=x;issuer=https://i;client_id=c;colour=blue", "unknown key");
    accepts("OPENCHIME_OIDC_CONNECT_2", "label=Work;issuer=https://login.example;client_id=abc;subject=oid");
    CHECK(oc_config_get()->n_connect == 1 && oc_config_get()->connect[0].subject_oid == 1);
    /* The audio token prefix is hex, in whole bytes. */
    refuses("OPENCHIME_AUDIO_TOKEN_PREFIX", "abc", "even number of hex");
    refuses("OPENCHIME_AUDIO_TOKEN_PREFIX", "zz", "hex");
    accepts("OPENCHIME_AUDIO_TOKEN_PREFIX", "0a0b");
    CHECK(oc_config_get()->audio_token_prefix_len == 2 && oc_config_get()->audio_token_prefix[1] == 0x0b);
    /* The trusted proxies parse. */
    refuses("OPENCHIME_TRUSTED_PROXIES", "not an address", "OPENCHIME_TRUSTED_PROXIES");
    accepts("OPENCHIME_TRUSTED_PROXIES", "127.0.0.0/8");
}

static void test_storage_policy(void) {
    /* Defaults, in the pass's units. */
    accepts("OPENCHIME_WORKSPACE_NAME", "x");
    const oc_config *c = oc_config_get();
    CHECK(c->storage.interval_ms == 5 * 60 * 1000);
    CHECK(c->storage.max_age_ms == 0);
    CHECK(c->storage.evict_enabled == 1);
    CHECK(c->storage.reserve_bytes == 256ull * 1024 * 1024);
    CHECK(c->storage.pressure_bytes >= c->storage.reserve_bytes);
    CHECK(c->storage.batch == 64);
    CHECK(c->storage.audit_max_age_ms == 365ull * 24 * 3600 * 1000);
    CHECK(c->sched_tick_ms == 15000);
    accepts("OPENCHIME_ATTACH_MAX_AGE_DAYS", "30");
    CHECK(oc_config_get()->storage.max_age_ms == 30ull * 24 * 60 * 60 * 1000);
    refuses("OPENCHIME_MAINT_INTERVAL_MS", "0", "1000..");           /* would busy-loop: refused, not floored */
    accepts("OPENCHIME_MAINT_INTERVAL_MS", "1000");
    CHECK(oc_config_get()->storage.interval_ms == 1000);
    accepts("OPENCHIME_SCHED_TICK_MS", "50");
    CHECK(oc_config_get()->sched_tick_ms == 50);
}

static void test_demo_switches(void) {
    /* The test binary is a demo build: the switches are read. */
    CHECK(oc_config_is_demo() == 1);
    accepts("OPENCHIME_TEST_PASSWORD_AUTH", "1");
    CHECK(oc_config_get()->demo.password_frames == 1);
    accepts("OPENCHIME_TEST_PASSWORD_AUTH", "off");
    CHECK(oc_config_get()->demo.password_frames == 0);
    accepts("OPENCHIME_UNFURL_ALLOW_PRIVATE", "1");
    CHECK(oc_config_get()->unfurl.allow_private == 1);
    accepts("OPENCHIME_FILE_PAGE", "2");
    CHECK(oc_config_get()->file_page == 2);
    refuses("OPENCHIME_FILE_PAGE", "0", "1..200");
    accepts("OPENCHIME_BOOTSTRAP_USERS", "alice:pw:owner");
    /* The clients' switches are known names, not stored. */
    accepts("OPENCHIME_TEST_AUDIO", "synthetic");
    accepts("OPENCHIME_TEST_DIR", "/tmp/x");
    /* A demo box enrols with a loopback test service only. */
    refuses("OPENCHIME_ENROLL_URL", "https://openchime.io", "loopback");
    accepts("OPENCHIME_DEPLOYMENT_MODE", "managed");       /* the fixtures call themselves managed */
}

static void test_reload_and_registry(void) {
    /* Loading twice gives the same answer and the file buffer is not leaked
     * (valgrind's job; here: the pointer is replaced, not stacked). */
    char err[512];
    CHECK(oc_config_load(err, sizeof err) == 0);
    const char *db1 = oc_config_get()->db_path;
    CHECK(oc_config_load(err, sizeof err) == 0);
    CHECK(!strcmp(db1, oc_config_get()->db_path));
    /* The factor key is beside the database unless named. */
    CHECK(strstr(oc_config_get()->factor_key_file, "/data/factor.key") != NULL);
    accepts("OPENCHIME_FACTOR_KEY_FILE", "/tmp/fk");
    CHECK(!strcmp(oc_config_get()->factor_key_file, "/tmp/fk"));
    /* Every row: a name, a group, a doc line; a class; a default that its own
     * kind accepts (a default that fails its parser is a bug in the table). */
    int rows = 0;
    for (const cfg_row *r = OC_CFG; r->name; r++, rows++) {
        CHECK(r->group && r->doc && r->doc[0]);
        CHECK(strncmp(r->name, "OPENCHIME_", 10) == 0);
        if (r->dflt && r->kind == CFG_INT) { long long v = atoll(r->dflt); CHECK(v >= r->min && v <= r->max); }
        if (r->dflt && r->kind == CFG_U64) { long long v = atoll(r->dflt); CHECK(v >= r->min && v <= r->max); }
        if (r->dflt && r->kind == CFG_BOOL) CHECK(!strcmp(r->dflt, "on") || !strcmp(r->dflt, "off"));
        if (r->dflt && r->kind == CFG_ENUM) CHECK(strstr(r->enums, r->dflt) != NULL);
        if (r->kind == CFG_CUSTOM) CHECK(r->parse != NULL);
        if (r->flags & CFG_PATTERN) CHECK(strstr(r->name, "%d") && r->count > 0);
    }
    CHECK(rows > 70);
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
          "  OPENCHIME_MAX_USERS='12'  \n"
          "export OPENCHIME_HEALTH_PORT=0\n", f);
    fclose(f);
    unsetenv("OPENCHIME_WORKSPACE_NAME"); unsetenv("OPENCHIME_MAX_USERS"); unsetenv("OPENCHIME_HEALTH_PORT");
    CHECK(oc_config_apply_env_file(path, err, sizeof err) == 0);
    CHECK(oc_config_load(err, sizeof err) == 0);
    CHECK(!strcmp(oc_config_get()->workspace_name, "Acme Inc"));   /* quotes stripped */
    CHECK(oc_config_get()->max_users == 12);
    CHECK(oc_config_get()->health_port == 0);
    unsetenv("OPENCHIME_WORKSPACE_NAME"); unsetenv("OPENCHIME_MAX_USERS"); unsetenv("OPENCHIME_HEALTH_PORT");
    /* A line that is not NAME=value names its line. */
    f = fopen(path, "w"); fputs("OPENCHIME_PROTO_PORT=8443\nthis is not a setting\n", f); fclose(f);
    CHECK(oc_config_apply_env_file(path, err, sizeof err) != 0 && strstr(err, ":2:"));
    unsetenv("OPENCHIME_PROTO_PORT");
    CHECK(oc_config_apply_env_file("/no/such/file", err, sizeof err) != 0);
    /* The packaged file is complete and loads. */
    CHECK(oc_config_apply_env_file("packaging/openchimed.env", err, sizeof err) == 0);
    CHECK(oc_config_load(err, sizeof err) == 0);
    if (oc_config_load(err, sizeof err) != 0) printf("    packaged env: %s\n", err);
    CHECK(!strcmp(oc_config_get()->db_path, "/var/lib/openchime/openchime.db"));
    for (const cfg_row *r = OC_CFG; r->name; r++) if (!(r->flags & CFG_PATTERN)) unsetenv(r->name);
    remove(path);
    /* Back to the suite's own configuration (tests/main.c). */
    setenv("OPENCHIME_TEST_PASSWORD_AUTH", "1", 1);
    CHECK(oc_config_load(err, sizeof err) == 0);
}

int run_config_tests(void) {
    printf("test_config: strict numbers, bools and enums; unknown and removed names; the cross-row rules; "
           "the storage policy; the demo switches; reload; the registry's own consistency; the EnvironmentFile\n");
    char err[512];
    CHECK(oc_config_load(err, sizeof err) == 0);
    test_numbers();
    test_bools_and_enums();
    test_unknown_and_removed();
    test_cross_rules();
    test_storage_policy();
    test_demo_switches();
    test_reload_and_registry();
    test_env_file();
    return failures;
}
