/* `signin <host> <port> <user> <password>`: sign in through the daemon's page
 * (tests/signin.h) and print the token and the verifier a client presents on
 * AUTH, space-separated, on one line. For a shell script that drives a client
 * which has no browser of its own. Exit 1 with the reason on stderr. */
#include "testsignin.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: signin <host> <port> <user> <password>\n"); return 2; }
    char tok[OC_TEST_TOKEN_CAP], ver[OC_TEST_VERIFIER_CAP], why[256];
    if (oc_test_signin(argv[1], atoi(argv[2]), NULL, argv[3], argv[4],
                       tok, sizeof tok, ver, sizeof ver, why, sizeof why) != 0) {
        fprintf(stderr, "signin: %s\n", why);
        return 1;
    }
    printf("%s %s\n", tok, ver);
    return 0;
}
