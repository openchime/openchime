/* Windows core-port smoke (ARCH-81). A console program that links the client
 * app-core and drives it against a running daemon: connect -> auth, printing the
 * outcome. It exercises exactly the seams a Windows build newly depends on — the
 * thread shim (oc_thread.h), Winsock (sock.h), DnsQuery/RNG, and mbedTLS TLS —
 * with no TUI and no console rendering, so it runs headless and its result is a
 * line of text rather than something to eyeball. Not part of `make test`.
 *
 *   win_smoke.exe <host> <port> <user:pass>
 */
#include "client.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  define SLEEP_MS(x) Sleep(x)
#else
#  include <unistd.h>
#  define SLEEP_MS(x) usleep((x) * 1000)
#endif

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s host port user:pass\n", argv[0]); return 2; }
    oc_client *c = oc_client_start(argv[1], atoi(argv[2]), argv[3]);
    if (!c) { printf("WIN CORE: start FAILED\n"); return 1; }

    int authed = 0, connected = 0, nchan = 0;
    for (int i = 0; i < 500; i++) {          /* up to ~10s */
        oc_client_tick(c);
        const oc_model *m = oc_client_model(c);
        if (m->connected) connected = 1;
        if (m->authed) { authed = 1; nchan = (int)m->n_channels; if (i > 80) break; }
        SLEEP_MS(20);
    }
    printf("WIN CORE: connected=%d authed=%d channels=%d\n", connected, authed, nchan);
    fflush(stdout);
    oc_client_stop(c);
    return authed ? 0 : 1;
}
