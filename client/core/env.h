/* The clients' few environment reads (CONFIG.md "Development mode only"), in
 * one place so scripts/check/config.sh can hold the rest of the
 * client to none. The development inputs (OPENCHIME_DEV_*) are read only when
 * OPENCHIME_MODE=development: an ordinary client ignores them, whatever is
 * set. In the browser the page sets OPENCHIME_MODE from the daemon that served
 * it, never from its address (openchime.html). */
#ifndef OC_ENV_H
#define OC_ENV_H

/* A client setting every client reads: the value, or NULL when unset or
 * empty. */
const char *oc_env(const char *name);

/* A development input (OPENCHIME_DEV_*): the value, or NULL when unset, empty,
 * or OPENCHIME_MODE is not development. */
const char *oc_env_dev(const char *name);

/* Whether OPENCHIME_MODE is development. */
int oc_env_is_dev(void);

#endif
