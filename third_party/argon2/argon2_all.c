/* The Argon2 reference implementation (P-H-C/phc-winner-argon2 20190702,
 * CC0 or Apache-2.0, LICENSE) as one translation unit, compiled with its own
 * flags into build/argon2.o (mk/vendor.mk): the portable ref.c, no threads
 * (the daemon hashes with one lane). Only the files are as released. */
#include "argon2.c"
#undef _DEFAULT_SOURCE      /* core.c defines it; string.h already has */
#include "core.c"
#include "encoding.c"
#include "ref.c"
#include "thread.c"
#undef G                    /* ref.c's round macro; blake2b.c has its own */
#include "blake2/blake2b.c"
