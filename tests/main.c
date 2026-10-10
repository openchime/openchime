/* Entry point for the single OpenChime test binary. Each suite lives in its own
 * translation unit under tests/, uses the shared CHECK macro (tests/check.h),
 * and exposes a run_<suite>_tests() returning its failure count. Built by
 * `make test`; a non-zero total exits non-zero and fails CI. In-process
 * integration suites (TLS, event loop) live here too; the black-box,
 * container-level end-to-end tests are separate (Scripts/test-integration.sh). */

#include <stddef.h>
#include <stdio.h>

int run_protocol_tests(void);
int run_callsig_tests(void);
int run_fuzz_tests(void);
int run_framebuf_tests(void);
int run_migrate_tests(void);
int run_auth_tests(void);
int run_jwt_tests(void);
int run_joinrules_tests(void);
int run_idtoken_tests(void);
int run_relaykeys_tests(void);
int run_totp_tests(void);
int run_webauthn_tests(void);
int run_proxyproto_tests(void);
int run_signin_tests(void);
int run_tkqr_tests(void);
int run_devicecodes_tests(void);
int run_ratelimit_tests(void);
int run_idmap_tests(void);
int run_srccount_tests(void);
int run_authpool_tests(void);
int run_ioloop_tests(void);
int run_roles_tests(void);
int run_dbwriter_tests(void);
int run_http_tests(void);
int run_sigv4_tests(void);
int run_blob_s3_tests(void);
int run_xferpool_tests(void);
int run_storage_tests(void);
int run_slow_blob_tests(void);
int run_audio_tests(void);
int run_media_tests(void);
int run_video_media_tests(void);
int run_emoji_tests(void);
int run_tls_tests(void);
int run_acme_tests(void);
int run_netloop_tests(void);
int run_client_core_tests(void);
int run_enroll_tests(void);
int run_push_tests(void);
int run_invite_mail_tests(void);
int run_mention_tests(void);
int run_searchq_tests(void);
int run_richtext_tests(void);
int run_url_tests(void);
int run_sock_tests(void);
int run_speakable_tests(void);
int run_action_tests(void);
int run_ttskit_tests(void);
int run_tts_worker_tests(void);
int run_summary_tests(void);
int run_voice_pick_tests(void);
int run_tts_data_tests(void);
int run_stt_tests(void);
int run_voice_tests(void);
int run_unfurl_tests(void);
int run_sdltext_map_tests(void);
int run_theme_tests(void);
int run_e2e_tests(void);
int run_call_media_tests(void);
int run_share_media_tests(void);

/* ---- when the binary itself dies ---------------------------------------------
 *
 * A crash used to leave nothing: standard output is block-buffered into CI's
 * pipe, so even the name of the suite running went down with the process, and
 * there was no backtrace -- one crash on CI was knowable only as "Segmentation
 * fault (core dumped)". So the output is line-buffered, the suite running is
 * named, and a fatal signal prints where it happened: the faulting address, the
 * crashing thread's stack resolved to file and line, and -- where gdb is
 * installed, as it is on CI -- every thread's stack, since the thread that
 * faults is often not the one that broke what it touched. Then the signal is
 * raised again, so the exit status is the one it would have been. */

#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *volatile g_suite = "(before the first suite)";
static struct timespec g_start;

static double elapsed_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)(t.tv_sec - g_start.tv_sec) + (double)(t.tv_nsec - g_start.tv_nsec) / 1e9;
}

/* Only what a signal handler may do, until the stacks are printed: the process
 * is already lost, but a handler that deadlocks in malloc prints nothing. */
static void say(const char *s) { ssize_t n = write(2, s, strlen(s)); (void)n; }

static void on_fatal(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char line[256];
    snprintf(line, sizeof line, "\n*** FATAL: %s in suite %s at %.1fs, address %p, thread %ld\n",
             strsignal(sig), g_suite, elapsed_s(), si ? si->si_addr : NULL, (long)gettid());
    say(line);
    /* The crashing thread's stack, resolved by addr2line against this binary. */
    void *pc[64];
    int n = backtrace(pc, 64);
    say("*** the crashing thread:\n");
    backtrace_symbols_fd(pc, n, 2);
    char cmd[4096];
    int at = snprintf(cmd, sizeof cmd, "addr2line -f -C -i -p -e /proc/%ld/exe", (long)getpid());
    char **sym = backtrace_symbols(pc, n);
    for (int i = 0; sym && i < n && at < (int)sizeof cmd - 40; i++) {
        /* "./build/tests(+0x1a2b3)" or "./build/tests(fn+0x10) [0x55..]": the
         * offset in the file is what addr2line wants for a position-independent
         * binary; a frame in another library is skipped. */
        const char *o = strstr(sym[i], "(+0x");
        if (!o || !strstr(sym[i], "tests")) continue;
        at += snprintf(cmd + at, sizeof cmd - at, " %.*s", (int)strcspn(o + 2, ")"), o + 2);
    }
    free(sym);
    say("*** resolved:\n");
    if (system(cmd) != 0) say("(addr2line failed)\n");
    /* Every thread, from gdb attached to this process, if there is a gdb. */
    pid_t me = getpid(), child = fork();
    if (child == 0) {
        char pid[32];
        snprintf(pid, sizeof pid, "%ld", (long)me);
        execlp("gdb", "gdb", "-p", pid, "-batch", "-nx", "-ex", "set pagination off",
               "-ex", "set debuginfod enabled off",
               "-ex", "thread apply all bt", (char *)NULL);
        say("(no gdb: only the crashing thread's stack above)\n");
        _exit(0);
    }
    if (child > 0) { int st; waitpid(child, &st, 0); }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void crash_report_install(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    clock_gettime(CLOCK_MONOTONIC, &g_start);
    /* gdb attaches from a child, which Yama's default ptrace scope allows only if
     * this process says so. */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    /* Its own stack, so a stack overflow is reported too. */
    static char alt[1 << 16];
    stack_t ss = { .ss_sp = alt, .ss_size = sizeof alt, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fatal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

/* Run one suite, or not: OC_TEST_ONLY is a comma-separated list of names to run
 * ("media,audio" runs run_media_tests and run_audio_tests), empty for all. With
 * OC_TEST_REPEAT the whole selection runs that many times. Both exist for
 * hunting a crash that appears once in fifty runs: the alternative is a
 * minute of unrelated suites per attempt. Unset, nothing changes. */
static const char *g_only;

/* OC_TEST_EXCEPT is the opposite list: the suites NOT to run. CI's build job
 * runs every suite its thread-sanitizer job does not (the Makefile's
 * TSAN_SUITES), so each suite runs once in CI, not twice. */
static const char *g_except;

static int listed(const char *list, const char *name) {
    for (const char *p = list; p && *p; ) {
        size_t n = strcspn(p, ",");
        if (n && memmem(name, strlen(name), p, n)) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

static int suite_wanted(const char *name) {
    if (g_except && *g_except && listed(g_except, name)) return 0;
    return !g_only || !*g_only || listed(g_only, name);
}

/* The suites a run selected, so a filter that names nothing fails rather than
 * passing empty. */
static int g_ran;

/* How long each suite took, printed slowest first at the end: the budget is
 * two minutes for everything, and this says where it goes. */
static struct { const char *name; double s; } g_times[128];
static int g_ntimes;

static void record_time(const char *name, double s) {
    if (g_ntimes < (int)(sizeof g_times / sizeof g_times[0])) {
        g_times[g_ntimes].name = name;
        g_times[g_ntimes].s = s;
        g_ntimes++;
    }
}

static int by_time(const void *a, const void *b) {
    double x = *(const double *)((const char *)a + offsetof(__typeof__(g_times[0]), s));
    double y = *(const double *)((const char *)b + offsetof(__typeof__(g_times[0]), s));
    return x < y ? 1 : x > y ? -1 : 0;
}

static void print_times(void) {
    qsort(g_times, (size_t)g_ntimes, sizeof g_times[0], by_time);
    printf("\nsuite times (%.1fs in all):\n", elapsed_s());
    for (int i = 0; i < g_ntimes; i++) printf("  %7.1fs  %s\n", g_times[i].s, g_times[i].name);
}

#define S(fn) { #fn, fn }
static const struct suite { const char *name; int (*fn)(void); } SUITES[] = {
    S(run_protocol_tests),
    S(run_fuzz_tests),
    S(run_framebuf_tests),
    S(run_migrate_tests),
    S(run_auth_tests),
    S(run_jwt_tests),
    S(run_joinrules_tests),
    S(run_idtoken_tests),
    S(run_relaykeys_tests),
    S(run_totp_tests),
    S(run_webauthn_tests),
    S(run_proxyproto_tests),
    S(run_signin_tests),
    S(run_tkqr_tests),
    S(run_devicecodes_tests),
    S(run_ratelimit_tests),
    S(run_idmap_tests),
    S(run_srccount_tests),
    S(run_authpool_tests),
    S(run_ioloop_tests),
    S(run_roles_tests),
    S(run_dbwriter_tests),
    S(run_http_tests),
    S(run_sigv4_tests),
    S(run_blob_s3_tests),
    S(run_xferpool_tests),
    S(run_storage_tests),
    S(run_slow_blob_tests),
    S(run_audio_tests),
    S(run_media_tests),
    S(run_video_media_tests),
    S(run_emoji_tests),
    S(run_tls_tests),
    S(run_acme_tests),
    S(run_netloop_tests),
    S(run_client_core_tests),
    S(run_callsig_tests),
    S(run_enroll_tests),
    S(run_push_tests),
    S(run_invite_mail_tests),
    S(run_mention_tests),
    S(run_searchq_tests),
    S(run_richtext_tests),
    S(run_url_tests),
    S(run_sock_tests),
    S(run_speakable_tests),
    S(run_action_tests),
    S(run_ttskit_tests),
    S(run_tts_worker_tests),
    S(run_summary_tests),
    S(run_voice_pick_tests),
    S(run_tts_data_tests),
    S(run_stt_tests),
    S(run_voice_tests),
    S(run_unfurl_tests),
    S(run_sdltext_map_tests),
    S(run_theme_tests),
    S(run_e2e_tests),
    S(run_call_media_tests),
    S(run_share_media_tests),
};
#undef S
enum { N_SUITES = sizeof SUITES / sizeof SUITES[0] };

/* One suite in this process, named first, so the output says what was running
 * when it died. */
static int run_here(const struct suite *su) {
    g_suite = su->name;
    double t0 = elapsed_s();
    int failed = su->fn();
    record_time(su->name, elapsed_s() - t0);
    return failed;
}

/* ---- suites side by side ------------------------------------------------------
 *
 * Nearly all of a run is waiting -- on timers, sockets, real-time audio and
 * video -- so OC_TEST_JOBS=N runs up to N suites at once, each in a process of
 * its own: what one suite does to its process (environment, test knobs, signal
 * handlers) stays there. A suite's output goes to a file and is printed whole
 * when it ends, so suites do not interleave. Each suite's databases are its
 * own, and the suites that listen do so on port ranges no other suite uses.
 * Unset or 1, the suites run one after another in this process. */

typedef struct { pid_t pid; const struct suite *su; FILE *out; int rd; double t0; } job;

/* Fork one suite. Its failure count and time come back on a pipe; a suite that
 * dies sends nothing, and its exit status says how it ended. */
static int job_start(job *j, const struct suite *su) {
    int fds[2];
    j->su = su; j->t0 = elapsed_s();
    j->out = tmpfile();
    if (!j->out || pipe(fds) != 0) { if (j->out) fclose(j->out); return -1; }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { fclose(j->out); close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fileno(j->out), STDOUT_FILENO);
        dup2(fileno(j->out), STDERR_FILENO);
        g_ntimes = 0;
        printf("--- %s\n", su->name);
        int failed = run_here(su);
        double t = g_ntimes ? g_times[0].s : 0;
        fflush(NULL);
        if (write(fds[1], &failed, sizeof failed) != (ssize_t)sizeof failed ||
            write(fds[1], &t, sizeof t) != (ssize_t)sizeof t) _exit(3);
        exit(0);
    }
    close(fds[1]);
    j->pid = pid; j->rd = fds[0];
    return 0;
}

/* Wait for one of the running suites to end; print its output and return its
 * failures (1 for one that died without saying). */
static int job_reap(job *jobs, int *n) {
    int status;
    pid_t pid = waitpid(-1, &status, 0);
    if (pid < 0) return 1;
    int k = 0;
    while (k < *n && jobs[k].pid != pid) k++;
    if (k == *n) return 0;
    job *j = &jobs[k];
    int failed = -1; double t = elapsed_s() - j->t0;
    if (read(j->rd, &failed, sizeof failed) != (ssize_t)sizeof failed || read(j->rd, &t, sizeof t) != (ssize_t)sizeof t)
        failed = -1;
    close(j->rd);
    fflush(stdout);
    rewind(j->out);
    char buf[8192]; size_t got;
    while ((got = fread(buf, 1, sizeof buf, j->out)) > 0) fwrite(buf, 1, got, stdout);
    fclose(j->out);
    if (failed < 0) {
        if (WIFSIGNALED(status)) printf("  FAIL %s died: %s\n", j->su->name, strsignal(WTERMSIG(status)));
        else printf("  FAIL %s exited %d without its result\n", j->su->name, WEXITSTATUS(status));
        failed = 1;
    }
    fflush(stdout);
    record_time(j->su->name, t);
    jobs[k] = jobs[--*n];
    return failed;
}

static int run_parallel(int max_jobs) {
    job jobs[64];
    if (max_jobs > (int)(sizeof jobs / sizeof jobs[0])) max_jobs = (int)(sizeof jobs / sizeof jobs[0]);
    int n = 0, total = 0;
    for (int i = 0; i < N_SUITES; i++) {
        if (!suite_wanted(SUITES[i].name)) continue;
        g_ran++;
        if (n == max_jobs) total += job_reap(jobs, &n);
        if (job_start(&jobs[n], &SUITES[i]) == 0) n++;
        else { printf("  FAIL %s: could not start\n", SUITES[i].name); total++; }
    }
    while (n > 0) total += job_reap(jobs, &n);
    return total;
}

int main(void) {
    crash_report_install();
    /* The suites sign in by password, in frames, as the test knob allows
     * (AUTH.md §8.10); the checks that the product refuses them turn it off
     * around themselves. */
    setenv("OPENCHIME_TEST_PASSWORD_AUTH", "1", 1);
    g_only = getenv("OC_TEST_ONLY");
    g_except = getenv("OC_TEST_EXCEPT");
    const char *rep = getenv("OC_TEST_REPEAT");
    int rounds = rep && *rep ? atoi(rep) : 1;
    if (rounds < 1) rounds = 1;
    const char *jv = getenv("OC_TEST_JOBS");
    int max_jobs = jv && *jv ? atoi(jv) : 1;
    int total = 0;
    for (int round = 0; round < rounds; round++) {
        if (rounds > 1) printf("=== round %d of %d\n", round + 1, rounds);
        if (max_jobs > 1) { total += run_parallel(max_jobs); continue; }
        for (int i = 0; i < N_SUITES; i++)
            if (suite_wanted(SUITES[i].name)) { g_ran++; total += run_here(&SUITES[i]); }
    }
    if (g_ran == 0) { printf("\nFAILED: the selection names no suite\n"); return 1; }
    print_times();
    if (total == 0) { printf("\nOK: all suites passed\n"); return 0; }
    printf("\nFAILED: %d check(s) across all suites\n", total);
    return 1;
}
