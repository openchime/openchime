/* The clients' environment reads (env.h). */
#include "env.h"

#include <stdlib.h>

const char *oc_env(const char *name) {
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

const char *oc_env_demo(const char *name) {
#ifdef OC_DEMO
    return oc_env(name);
#else
    (void)name;
    return NULL;
#endif
}

/* A demo build carries this string and a production build does not, so the
 * release can tell them apart without running the binary (release.yml). */
#ifdef OC_DEMO
static const volatile char DEMO_MARKER[] = "OPENCHIME-DEMO-BUILD";
int oc_env_is_demo(void) { return DEMO_MARKER[0] != 0; }
#else
int oc_env_is_demo(void) { return 0; }
#endif
