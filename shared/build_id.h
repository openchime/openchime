/*
 * The build id (ARCH-20, docs/UPGRADING.md): the short commit a candidate build
 * compiled, stamped by the build (-DOC_BUILD_ID=...). It is the binary's
 * identity. A release number is a label assigned to a build afterwards and is
 * not in any binary; the package that carries the binary says it. A source
 * build that sets nothing reports "dev", which is the honest answer, and lets
 * an operator tell a running process from an installed package.
 *
 * One fallback for every binary that says its build: the daemon (`--version`,
 * the startup log, SERVER_BUILD to a signed-in client), the TUI (`--version`)
 * and the GUI clients (About OpenChime).
 */

#ifndef OC_BUILD_ID_H
#define OC_BUILD_ID_H

#ifndef OC_BUILD_ID
#define OC_BUILD_ID "dev"
#endif

#endif
