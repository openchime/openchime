/* Tenant role policy — see roles.h. Pure functions over OC_ROLE_* (protocol.h). */

#include "roles.h"
#include "protocol.h"

int oc_role_can_set_role(uint8_t actor, uint8_t cur, uint8_t next) {
    if (next != OC_ROLE_MEMBER && next != OC_ROLE_ADMIN && next != OC_ROLE_OWNER)
        return 0;                              /* not a valid role */
    if (actor == OC_ROLE_OWNER)
        return 1;                              /* owner may set any role */
    if (actor == OC_ROLE_ADMIN) {
        if (next == OC_ROLE_OWNER) return 0;   /* only an owner grants owner */
        if (cur == OC_ROLE_OWNER || cur == OC_ROLE_ADMIN) return 0; /* not over peers/owners */
        return 1;                              /* may promote/keep a member */
    }
    return 0;                                  /* members cannot change roles */
}

int oc_role_can_moderate(uint8_t actor) {
    return actor == OC_ROLE_OWNER || actor == OC_ROLE_ADMIN;
}

int oc_role_can_manage_members(uint8_t actor) {
    return actor == OC_ROLE_OWNER || actor == OC_ROLE_ADMIN;
}
