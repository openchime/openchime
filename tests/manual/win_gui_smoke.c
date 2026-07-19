/* GUI-path core smoke (ARCH-80): mirrors client/win32 do_connect exactly —
 * oc_resolve(workspace) then oc_client_start_secure(host,port,"user:pass",NULL,NULL)
 * — so the Connect button's path is verified headless. Not part of `make test`.
 *   win_gui_smoke.exe <workspace> <user:pass>
 */
#include "client.h"
#include "model.h"
#include "resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s workspace user:pass\n", argv[0]); return 2; }
    oc_endpoint ep;
    if (oc_resolve(argv[1], getenv("OPENCHIME_SUFFIX"), &ep) != OC_RESOLVE_OK) {
        printf("WIN GUI: resolve FAILED for '%s'\n", argv[1]); return 1;
    }
    printf("WIN GUI: resolved %s -> %s:%d\n", argv[1], ep.host, ep.port);
    oc_client *c = oc_client_start_secure(ep.host, ep.port, argv[2], NULL, NULL);
    if (!c) { printf("WIN GUI: start_secure FAILED\n"); return 1; }
    int authed = 0, connected = 0, nchan = 0;
    for (int i = 0; i < 500; i++) {
        oc_client_tick(c);
        const oc_model *m = oc_client_model(c);
        if (m->connected) connected = 1;
        if (m->authed) { authed = 1; nchan = (int)m->n_channels; if (i > 80) break; }
        Sleep(20);
    }
    printf("WIN GUI: connected=%d authed=%d channels=%d\n", connected, authed, nchan);
    fflush(stdout);
    oc_client_stop(c);
    return authed ? 0 : 1;
}
