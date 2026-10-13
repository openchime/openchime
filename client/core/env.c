/* The clients' environment reads (env.h). */
#include "env.h"

#include <stdlib.h>
#include <string.h>

const char *oc_env(const char *name) {
    const char *v = getenv(name);
    return v && *v ? v : NULL;
}

int oc_env_is_dev(void) {
    const char *m = oc_env("OPENCHIME_MODE");
    return m && strcmp(m, "development") == 0;
}

const char *oc_env_dev(const char *name) {
    return oc_env_is_dev() ? oc_env(name) : NULL;
}
