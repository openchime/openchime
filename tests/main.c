/* Entry point for the single OpenChime test binary. Each suite lives in its own
 * translation unit under tests/, uses the shared CHECK macro (tests/check.h),
 * and exposes a run_<suite>_tests() returning its failure count. Built by
 * `make test`; a non-zero total exits non-zero and fails CI. In-process
 * integration suites (TLS, event loop) live here too; the black-box,
 * container-level end-to-end tests are separate (Scripts/test-integration.sh). */

#include <stdio.h>

int run_protocol_tests(void);
int run_fuzz_tests(void);
int run_framebuf_tests(void);
int run_migrate_tests(void);
int run_auth_tests(void);
int run_jwt_tests(void);
int run_joinrules_tests(void);
int run_proxyproto_tests(void);
int run_signin_tests(void);
int run_ratelimit_tests(void);
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
int run_netloop_tests(void);
int run_client_core_tests(void);
int run_enroll_tests(void);
int run_push_tests(void);
int run_mention_tests(void);
int run_searchq_tests(void);
int run_richtext_tests(void);
int run_url_tests(void);
int run_speakable_tests(void);
int run_ttskit_tests(void);
int run_tts_worker_tests(void);
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
 * hunting a crash that appears once in fifty runs: the alternative is four
 * minutes of unrelated suites per attempt. Unset, nothing changes. */
static const char *g_only;

static int suite_wanted(const char *name) {
    if (!g_only || !*g_only) return 1;
    for (const char *p = g_only; *p; ) {
        size_t n = strcspn(p, ",");
        if (n && strstr(name, "") && memmem(name, strlen(name), p, n)) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

/* One suite, named first, so the output says what was running when it died. */
#define SUITE(fn) (suite_wanted(#fn) ? (g_suite = #fn, fn()) : 0)

int main(void) {
    crash_report_install();
    g_only = getenv("OC_TEST_ONLY");
    const char *rep = getenv("OC_TEST_REPEAT");
    int rounds = rep && *rep ? atoi(rep) : 1;
    if (rounds < 1) rounds = 1;
    int total = 0;
    for (int round = 0; round < rounds; round++) {
    if (rounds > 1) printf("=== round %d of %d\n", round + 1, rounds);
    total += SUITE(run_protocol_tests);
    total += SUITE(run_fuzz_tests);
    total += SUITE(run_framebuf_tests);
    total += SUITE(run_migrate_tests);
    total += SUITE(run_auth_tests);
    total += SUITE(run_jwt_tests);
    total += SUITE(run_joinrules_tests);
    total += SUITE(run_proxyproto_tests);
    total += SUITE(run_signin_tests);
    total += SUITE(run_ratelimit_tests);
    total += SUITE(run_roles_tests);
    total += SUITE(run_dbwriter_tests);
    total += SUITE(run_http_tests);
    total += SUITE(run_sigv4_tests);
    total += SUITE(run_blob_s3_tests);
    total += SUITE(run_xferpool_tests);
    total += SUITE(run_storage_tests);
    total += SUITE(run_slow_blob_tests);
    total += SUITE(run_audio_tests);
    total += SUITE(run_media_tests);
    total += SUITE(run_video_media_tests);
    total += SUITE(run_emoji_tests);
    total += SUITE(run_tls_tests);
    total += SUITE(run_netloop_tests);
    total += SUITE(run_client_core_tests);
    total += SUITE(run_enroll_tests);
    total += SUITE(run_push_tests);
    total += SUITE(run_mention_tests);
    total += SUITE(run_searchq_tests);
    total += SUITE(run_richtext_tests);
    total += SUITE(run_url_tests);
    total += SUITE(run_speakable_tests);
    total += SUITE(run_ttskit_tests);
    total += SUITE(run_tts_worker_tests);
    total += SUITE(run_voice_pick_tests);
    total += SUITE(run_tts_data_tests);
    total += SUITE(run_stt_tests);
    total += SUITE(run_voice_tests);
    total += SUITE(run_unfurl_tests);
    total += SUITE(run_sdltext_map_tests);
    total += SUITE(run_theme_tests);
    total += SUITE(run_e2e_tests);
    total += SUITE(run_call_media_tests);
    total += SUITE(run_share_media_tests);

    }
    if (total == 0) { printf("\nOK: all suites passed\n"); return 0; }
    printf("\nFAILED: %d check(s) across all suites\n", total);
    return 1;
}
