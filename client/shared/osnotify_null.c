/*
 * OpenChime — OS notifications where the platform's own are not built yet
 * (osnotify.h). Nothing can be raised: every call says so, and the frontend's
 * fallback and the badges carry the notification instead.
 */

#include "osnotify.h"

int      oc_osn_init(const char *app_id, const char *display_name, void *window, unsigned tray_id,
                     oc_osn_action_cb cb) {
    (void)app_id; (void)display_name; (void)window; (void)tray_id; (void)cb;
    return 0;
}
int      oc_osn_available(void) { return 0; }
unsigned oc_osn_caps(void) { return 0; }
int      oc_osn_show(const oc_osn *n) { (void)n; return OC_OSN_NOT_SHOWN; }
int      oc_osn_withdraw(const char *key, const char *group) { (void)key; (void)group; return 0; }
int      oc_osn_withdraw_group(const char *group) { (void)group; return 0; }
void     oc_osn_test_unavailable(int on) { (void)on; }
void     oc_osn_done(void) {}
