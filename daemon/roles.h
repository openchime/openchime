/*
 * Tenant role policy (ARCH-60, AUTH.md §6). Pure predicates over the OC_ROLE_*
 * values, kept separate from the DB write path so the rules are unit-testable in
 * isolation. The ≥1-owner invariant (REQ-030) is a DB-state check and lives in
 * the writer, not here — these functions decide only what a role is *allowed*
 * to do, given the actor's and target's roles.
 */

#ifndef OPENCHIME_ROLES_H
#define OPENCHIME_ROLES_H

#include <stdint.h>

/* May `actor` change a user whose current role is `cur` to `next`?
 *  - members cannot change any role;
 *  - only an owner may grant or revoke the owner role;
 *  - an admin may only promote/keep a *member* (never touch admins or owners);
 *  - an owner may set any role (the last-owner invariant is enforced separately).
 * `next` must be a valid role. */
int oc_role_can_set_role(uint8_t actor, uint8_t cur, uint8_t next);

/* May `actor` moderate (delete others' messages, REQ-032)? owner or admin. */
int oc_role_can_moderate(uint8_t actor);

/* May `actor` invite/remove tenant members (REQ-033)? owner or admin. */
int oc_role_can_manage_members(uint8_t actor);

#endif /* OPENCHIME_ROLES_H */
