/* Unit tests for the tenant role policy (daemon/roles.c) — pure predicates,
 * no DB. The ≥1-owner invariant is exercised at the dbwriter level
 * (test_dbwriter's SET_ROLE test), since it depends on DB state. */

#include "roles.h"
#include "protocol.h"
#include "check.h"

static void test_can_set_role(void) {
    /* Owner may set any valid role. */
    CHECK(oc_role_can_set_role(OC_ROLE_OWNER, OC_ROLE_MEMBER, OC_ROLE_ADMIN) == 1);
    CHECK(oc_role_can_set_role(OC_ROLE_OWNER, OC_ROLE_ADMIN, OC_ROLE_OWNER) == 1);
    CHECK(oc_role_can_set_role(OC_ROLE_OWNER, OC_ROLE_OWNER, OC_ROLE_MEMBER) == 1);

    /* Admin may promote/keep a member, but never touch admins or owners, nor
     * grant the owner role. */
    CHECK(oc_role_can_set_role(OC_ROLE_ADMIN, OC_ROLE_MEMBER, OC_ROLE_ADMIN) == 1);
    CHECK(oc_role_can_set_role(OC_ROLE_ADMIN, OC_ROLE_MEMBER, OC_ROLE_OWNER) == 0);
    CHECK(oc_role_can_set_role(OC_ROLE_ADMIN, OC_ROLE_ADMIN, OC_ROLE_MEMBER) == 0);
    CHECK(oc_role_can_set_role(OC_ROLE_ADMIN, OC_ROLE_OWNER, OC_ROLE_MEMBER) == 0);

    /* Members cannot change any role. */
    CHECK(oc_role_can_set_role(OC_ROLE_MEMBER, OC_ROLE_MEMBER, OC_ROLE_ADMIN) == 0);

    /* An invalid target role is rejected regardless of actor. */
    CHECK(oc_role_can_set_role(OC_ROLE_OWNER, OC_ROLE_MEMBER, 99) == 0);
}

static void test_capabilities(void) {
    CHECK(oc_role_can_moderate(OC_ROLE_OWNER) == 1);
    CHECK(oc_role_can_moderate(OC_ROLE_ADMIN) == 1);
    CHECK(oc_role_can_moderate(OC_ROLE_MEMBER) == 0);

    CHECK(oc_role_can_manage_members(OC_ROLE_OWNER) == 1);
    CHECK(oc_role_can_manage_members(OC_ROLE_ADMIN) == 1);
    CHECK(oc_role_can_manage_members(OC_ROLE_MEMBER) == 0);
}

int run_roles_tests(void) {
    printf("test_roles: set-role policy (owner/admin/member matrix), moderate/manage capabilities\n");
    test_can_set_role();
    test_capabilities();
    return failures;
}
