/*
 * OpenChime TUI — the platform secret-store backend (libsecret on Linux). Builds
 * an oc_secret vtable the core uses to keep the session token in the OS keyring.
 */

#ifndef OC_TUI_SECRET_BACKEND_H
#define OC_TUI_SECRET_BACKEND_H

#include "secret.h"

/* Open the keyring backend for `service`, or NULL when no keyring is available
 * (headless / no D-Bus session / not compiled with libsecret) — the caller then
 * falls back to the SQLite store. Free the result with oc_secret_free(). */
oc_secret *oc_tui_secret_open(const char *service);

#endif /* OC_TUI_SECRET_BACKEND_H */
