/*
 * OpenChime — the platform credential store, one entry point for every frontend.
 *
 * The core defines the oc_secret vtable (client/core/secret.h) and links no
 * keyring library; a frontend supplies the backend. This header is the single
 * name frontends call, so a GUI and a TUI on the same platform cannot end up
 * with different credential storage:
 *
 *   Windows   Credential Manager  (client/shared/secret_win.c)
 *   Linux     libsecret / Secret Service  (client/shared/secret_libsecret.c)
 *   macOS     Keychain  (later, same seam)
 *
 * Returns NULL when the platform store is unavailable — no libsecret at build
 * time, no D-Bus session, a locked keychain. **A NULL result means credentials
 * cannot be persisted at all**: the store refuses to write a session token to
 * SQLite (see oc_store_set_secret), so the user signs in again next launch.
 * That is deliberate — a credential is never written to a plaintext file.
 */

#ifndef OC_SECRET_OS_H
#define OC_SECRET_OS_H

#include "secret.h"

/* Open the platform credential store for `service` ("openchime"), or NULL if
 * this machine has none. Free with oc_secret_free(). */
oc_secret *oc_secret_open_os(const char *service);

#endif /* OC_SECRET_OS_H */
