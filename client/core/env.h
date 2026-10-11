/* The clients' few environment reads (CONFIG.md "Client-side", "Demo builds
 * only"), in one place so scripts/check/config.sh can hold the rest of the
 * client to none. The test switches exist in a demo build (make DEMO=1) and
 * not otherwise: a production client never reads OPENCHIME_TEST_*, whatever
 * is set, because the code behind them is not in it. */
#ifndef OC_ENV_H
#define OC_ENV_H

/* A client setting every build reads (OPENCHIME_SUFFIX): the value, or NULL
 * when unset or empty. */
const char *oc_env(const char *name);

/* A demo build's test switch (OPENCHIME_TEST_*): the value, or NULL when
 * unset, empty, or this is not a demo build. */
const char *oc_env_demo(const char *name);

/* Whether this binary is a demo build. */
int oc_env_is_demo(void);

#endif
