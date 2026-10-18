/* Tests for the end-to-end encryption of calls (shared/e2e_hpke.c,
 * shared/e2e_sframe.c; ARCH-113, CALLS.md §5): both constructions against their
 * RFC's test vectors, then what the vectors do not cover. */
#include "check.h"
#include "e2e_hpke.h"
#include "e2e_sframe.h"

#include <stdint.h>
#include <string.h>

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        unsigned v = 0;
        for (int i = 0; i < 2; i++) {
            char c = s[i];
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        out[n++] = (uint8_t)v;
    }
    return n;
}

/* RFC 9605 Appendix C.1: every header encoding. */
static const struct { uint64_t kid, ctr; const char *hdr; } HDRS[] = {
    { 0x0000000000000000ull, 0x0000000000000000ull, "00" },
    { 0x0000000000000000ull, 0x0000000000000001ull, "01" },
    { 0x0000000000000000ull, 0x00000000000000ffull, "08ff" },
    { 0x0000000000000000ull, 0x0000000000000100ull, "090100" },
    { 0x0000000000000000ull, 0x000000000000ffffull, "09ffff" },
    { 0x0000000000000000ull, 0x0000000000010000ull, "0a010000" },
    { 0x0000000000000000ull, 0x0000000000ffffffull, "0affffff" },
    { 0x0000000000000000ull, 0x0000000001000000ull, "0b01000000" },
    { 0x0000000000000000ull, 0x00000000ffffffffull, "0bffffffff" },
    { 0x0000000000000000ull, 0x0000000100000000ull, "0c0100000000" },
    { 0x0000000000000000ull, 0x000000ffffffffffull, "0cffffffffff" },
    { 0x0000000000000000ull, 0x0000010000000000ull, "0d010000000000" },
    { 0x0000000000000000ull, 0x0000ffffffffffffull, "0dffffffffffff" },
    { 0x0000000000000000ull, 0x0001000000000000ull, "0e01000000000000" },
    { 0x0000000000000000ull, 0x00ffffffffffffffull, "0effffffffffffff" },
    { 0x0000000000000000ull, 0x0100000000000000ull, "0f0100000000000000" },
    { 0x0000000000000000ull, 0xffffffffffffffffull, "0fffffffffffffffff" },
    { 0x0000000000000001ull, 0x0000000000000000ull, "10" },
    { 0x0000000000000001ull, 0x0000000000000001ull, "11" },
    { 0x0000000000000001ull, 0x00000000000000ffull, "18ff" },
    { 0x0000000000000001ull, 0x0000000000000100ull, "190100" },
    { 0x0000000000000001ull, 0x000000000000ffffull, "19ffff" },
    { 0x0000000000000001ull, 0x0000000000010000ull, "1a010000" },
    { 0x0000000000000001ull, 0x0000000000ffffffull, "1affffff" },
    { 0x0000000000000001ull, 0x0000000001000000ull, "1b01000000" },
    { 0x0000000000000001ull, 0x00000000ffffffffull, "1bffffffff" },
    { 0x0000000000000001ull, 0x0000000100000000ull, "1c0100000000" },
    { 0x0000000000000001ull, 0x000000ffffffffffull, "1cffffffffff" },
    { 0x0000000000000001ull, 0x0000010000000000ull, "1d010000000000" },
    { 0x0000000000000001ull, 0x0000ffffffffffffull, "1dffffffffffff" },
    { 0x0000000000000001ull, 0x0001000000000000ull, "1e01000000000000" },
    { 0x0000000000000001ull, 0x00ffffffffffffffull, "1effffffffffffff" },
    { 0x0000000000000001ull, 0x0100000000000000ull, "1f0100000000000000" },
    { 0x0000000000000001ull, 0xffffffffffffffffull, "1fffffffffffffffff" },
    { 0x00000000000000ffull, 0x0000000000000000ull, "80ff" },
    { 0x00000000000000ffull, 0x0000000000000001ull, "81ff" },
    { 0x00000000000000ffull, 0x00000000000000ffull, "88ffff" },
    { 0x00000000000000ffull, 0x0000000000000100ull, "89ff0100" },
    { 0x00000000000000ffull, 0x000000000000ffffull, "89ffffff" },
    { 0x00000000000000ffull, 0x0000000000010000ull, "8aff010000" },
    { 0x00000000000000ffull, 0x0000000000ffffffull, "8affffffff" },
    { 0x00000000000000ffull, 0x0000000001000000ull, "8bff01000000" },
    { 0x00000000000000ffull, 0x00000000ffffffffull, "8bffffffffff" },
    { 0x00000000000000ffull, 0x0000000100000000ull, "8cff0100000000" },
    { 0x00000000000000ffull, 0x000000ffffffffffull, "8cffffffffffff" },
    { 0x00000000000000ffull, 0x0000010000000000ull, "8dff010000000000" },
    { 0x00000000000000ffull, 0x0000ffffffffffffull, "8dffffffffffffff" },
    { 0x00000000000000ffull, 0x0001000000000000ull, "8eff01000000000000" },
    { 0x00000000000000ffull, 0x00ffffffffffffffull, "8effffffffffffffff" },
    { 0x00000000000000ffull, 0x0100000000000000ull, "8fff0100000000000000" },
    { 0x00000000000000ffull, 0xffffffffffffffffull, "8fffffffffffffffffff" },
    { 0x0000000000000100ull, 0x0000000000000000ull, "900100" },
    { 0x0000000000000100ull, 0x0000000000000001ull, "910100" },
    { 0x0000000000000100ull, 0x00000000000000ffull, "980100ff" },
    { 0x0000000000000100ull, 0x0000000000000100ull, "9901000100" },
    { 0x0000000000000100ull, 0x000000000000ffffull, "990100ffff" },
    { 0x0000000000000100ull, 0x0000000000010000ull, "9a0100010000" },
    { 0x0000000000000100ull, 0x0000000000ffffffull, "9a0100ffffff" },
    { 0x0000000000000100ull, 0x0000000001000000ull, "9b010001000000" },
    { 0x0000000000000100ull, 0x00000000ffffffffull, "9b0100ffffffff" },
    { 0x0000000000000100ull, 0x0000000100000000ull, "9c01000100000000" },
    { 0x0000000000000100ull, 0x000000ffffffffffull, "9c0100ffffffffff" },
    { 0x0000000000000100ull, 0x0000010000000000ull, "9d0100010000000000" },
    { 0x0000000000000100ull, 0x0000ffffffffffffull, "9d0100ffffffffffff" },
    { 0x0000000000000100ull, 0x0001000000000000ull, "9e010001000000000000" },
    { 0x0000000000000100ull, 0x00ffffffffffffffull, "9e0100ffffffffffffff" },
    { 0x0000000000000100ull, 0x0100000000000000ull, "9f01000100000000000000" },
    { 0x0000000000000100ull, 0xffffffffffffffffull, "9f0100ffffffffffffffff" },
    { 0x000000000000ffffull, 0x0000000000000000ull, "90ffff" },
    { 0x000000000000ffffull, 0x0000000000000001ull, "91ffff" },
    { 0x000000000000ffffull, 0x00000000000000ffull, "98ffffff" },
    { 0x000000000000ffffull, 0x0000000000000100ull, "99ffff0100" },
    { 0x000000000000ffffull, 0x000000000000ffffull, "99ffffffff" },
    { 0x000000000000ffffull, 0x0000000000010000ull, "9affff010000" },
    { 0x000000000000ffffull, 0x0000000000ffffffull, "9affffffffff" },
    { 0x000000000000ffffull, 0x0000000001000000ull, "9bffff01000000" },
    { 0x000000000000ffffull, 0x00000000ffffffffull, "9bffffffffffff" },
    { 0x000000000000ffffull, 0x0000000100000000ull, "9cffff0100000000" },
    { 0x000000000000ffffull, 0x000000ffffffffffull, "9cffffffffffffff" },
    { 0x000000000000ffffull, 0x0000010000000000ull, "9dffff010000000000" },
    { 0x000000000000ffffull, 0x0000ffffffffffffull, "9dffffffffffffffff" },
    { 0x000000000000ffffull, 0x0001000000000000ull, "9effff01000000000000" },
    { 0x000000000000ffffull, 0x00ffffffffffffffull, "9effffffffffffffffff" },
    { 0x000000000000ffffull, 0x0100000000000000ull, "9fffff0100000000000000" },
    { 0x000000000000ffffull, 0xffffffffffffffffull, "9fffffffffffffffffffff" },
    { 0x0000000000010000ull, 0x0000000000000000ull, "a0010000" },
    { 0x0000000000010000ull, 0x0000000000000001ull, "a1010000" },
    { 0x0000000000010000ull, 0x00000000000000ffull, "a8010000ff" },
    { 0x0000000000010000ull, 0x0000000000000100ull, "a90100000100" },
    { 0x0000000000010000ull, 0x000000000000ffffull, "a9010000ffff" },
    { 0x0000000000010000ull, 0x0000000000010000ull, "aa010000010000" },
    { 0x0000000000010000ull, 0x0000000000ffffffull, "aa010000ffffff" },
    { 0x0000000000010000ull, 0x0000000001000000ull, "ab01000001000000" },
    { 0x0000000000010000ull, 0x00000000ffffffffull, "ab010000ffffffff" },
    { 0x0000000000010000ull, 0x0000000100000000ull, "ac0100000100000000" },
    { 0x0000000000010000ull, 0x000000ffffffffffull, "ac010000ffffffffff" },
    { 0x0000000000010000ull, 0x0000010000000000ull, "ad010000010000000000" },
    { 0x0000000000010000ull, 0x0000ffffffffffffull, "ad010000ffffffffffff" },
    { 0x0000000000010000ull, 0x0001000000000000ull, "ae01000001000000000000" },
    { 0x0000000000010000ull, 0x00ffffffffffffffull, "ae010000ffffffffffffff" },
    { 0x0000000000010000ull, 0x0100000000000000ull, "af0100000100000000000000" },
    { 0x0000000000010000ull, 0xffffffffffffffffull, "af010000ffffffffffffffff" },
    { 0x0000000000ffffffull, 0x0000000000000000ull, "a0ffffff" },
    { 0x0000000000ffffffull, 0x0000000000000001ull, "a1ffffff" },
    { 0x0000000000ffffffull, 0x00000000000000ffull, "a8ffffffff" },
    { 0x0000000000ffffffull, 0x0000000000000100ull, "a9ffffff0100" },
    { 0x0000000000ffffffull, 0x000000000000ffffull, "a9ffffffffff" },
    { 0x0000000000ffffffull, 0x0000000000010000ull, "aaffffff010000" },
    { 0x0000000000ffffffull, 0x0000000000ffffffull, "aaffffffffffff" },
    { 0x0000000000ffffffull, 0x0000000001000000ull, "abffffff01000000" },
    { 0x0000000000ffffffull, 0x00000000ffffffffull, "abffffffffffffff" },
    { 0x0000000000ffffffull, 0x0000000100000000ull, "acffffff0100000000" },
    { 0x0000000000ffffffull, 0x000000ffffffffffull, "acffffffffffffffff" },
    { 0x0000000000ffffffull, 0x0000010000000000ull, "adffffff010000000000" },
    { 0x0000000000ffffffull, 0x0000ffffffffffffull, "adffffffffffffffffff" },
    { 0x0000000000ffffffull, 0x0001000000000000ull, "aeffffff01000000000000" },
    { 0x0000000000ffffffull, 0x00ffffffffffffffull, "aeffffffffffffffffffff" },
    { 0x0000000000ffffffull, 0x0100000000000000ull, "afffffff0100000000000000" },
    { 0x0000000000ffffffull, 0xffffffffffffffffull, "afffffffffffffffffffffff" },
    { 0x0000000001000000ull, 0x0000000000000000ull, "b001000000" },
    { 0x0000000001000000ull, 0x0000000000000001ull, "b101000000" },
    { 0x0000000001000000ull, 0x00000000000000ffull, "b801000000ff" },
    { 0x0000000001000000ull, 0x0000000000000100ull, "b9010000000100" },
    { 0x0000000001000000ull, 0x000000000000ffffull, "b901000000ffff" },
    { 0x0000000001000000ull, 0x0000000000010000ull, "ba01000000010000" },
    { 0x0000000001000000ull, 0x0000000000ffffffull, "ba01000000ffffff" },
    { 0x0000000001000000ull, 0x0000000001000000ull, "bb0100000001000000" },
    { 0x0000000001000000ull, 0x00000000ffffffffull, "bb01000000ffffffff" },
    { 0x0000000001000000ull, 0x0000000100000000ull, "bc010000000100000000" },
    { 0x0000000001000000ull, 0x000000ffffffffffull, "bc01000000ffffffffff" },
    { 0x0000000001000000ull, 0x0000010000000000ull, "bd01000000010000000000" },
    { 0x0000000001000000ull, 0x0000ffffffffffffull, "bd01000000ffffffffffff" },
    { 0x0000000001000000ull, 0x0001000000000000ull, "be0100000001000000000000" },
    { 0x0000000001000000ull, 0x00ffffffffffffffull, "be01000000ffffffffffffff" },
    { 0x0000000001000000ull, 0x0100000000000000ull, "bf010000000100000000000000" },
    { 0x0000000001000000ull, 0xffffffffffffffffull, "bf01000000ffffffffffffffff" },
    { 0x00000000ffffffffull, 0x0000000000000000ull, "b0ffffffff" },
    { 0x00000000ffffffffull, 0x0000000000000001ull, "b1ffffffff" },
    { 0x00000000ffffffffull, 0x00000000000000ffull, "b8ffffffffff" },
    { 0x00000000ffffffffull, 0x0000000000000100ull, "b9ffffffff0100" },
    { 0x00000000ffffffffull, 0x000000000000ffffull, "b9ffffffffffff" },
    { 0x00000000ffffffffull, 0x0000000000010000ull, "baffffffff010000" },
    { 0x00000000ffffffffull, 0x0000000000ffffffull, "baffffffffffffff" },
    { 0x00000000ffffffffull, 0x0000000001000000ull, "bbffffffff01000000" },
    { 0x00000000ffffffffull, 0x00000000ffffffffull, "bbffffffffffffffff" },
    { 0x00000000ffffffffull, 0x0000000100000000ull, "bcffffffff0100000000" },
    { 0x00000000ffffffffull, 0x000000ffffffffffull, "bcffffffffffffffffff" },
    { 0x00000000ffffffffull, 0x0000010000000000ull, "bdffffffff010000000000" },
    { 0x00000000ffffffffull, 0x0000ffffffffffffull, "bdffffffffffffffffffff" },
    { 0x00000000ffffffffull, 0x0001000000000000ull, "beffffffff01000000000000" },
    { 0x00000000ffffffffull, 0x00ffffffffffffffull, "beffffffffffffffffffffff" },
    { 0x00000000ffffffffull, 0x0100000000000000ull, "bfffffffff0100000000000000" },
    { 0x00000000ffffffffull, 0xffffffffffffffffull, "bfffffffffffffffffffffffff" },
    { 0x0000000100000000ull, 0x0000000000000000ull, "c00100000000" },
    { 0x0000000100000000ull, 0x0000000000000001ull, "c10100000000" },
    { 0x0000000100000000ull, 0x00000000000000ffull, "c80100000000ff" },
    { 0x0000000100000000ull, 0x0000000000000100ull, "c901000000000100" },
    { 0x0000000100000000ull, 0x000000000000ffffull, "c90100000000ffff" },
    { 0x0000000100000000ull, 0x0000000000010000ull, "ca0100000000010000" },
    { 0x0000000100000000ull, 0x0000000000ffffffull, "ca0100000000ffffff" },
    { 0x0000000100000000ull, 0x0000000001000000ull, "cb010000000001000000" },
    { 0x0000000100000000ull, 0x00000000ffffffffull, "cb0100000000ffffffff" },
    { 0x0000000100000000ull, 0x0000000100000000ull, "cc01000000000100000000" },
    { 0x0000000100000000ull, 0x000000ffffffffffull, "cc0100000000ffffffffff" },
    { 0x0000000100000000ull, 0x0000010000000000ull, "cd0100000000010000000000" },
    { 0x0000000100000000ull, 0x0000ffffffffffffull, "cd0100000000ffffffffffff" },
    { 0x0000000100000000ull, 0x0001000000000000ull, "ce010000000001000000000000" },
    { 0x0000000100000000ull, 0x00ffffffffffffffull, "ce0100000000ffffffffffffff" },
    { 0x0000000100000000ull, 0x0100000000000000ull, "cf01000000000100000000000000" },
    { 0x0000000100000000ull, 0xffffffffffffffffull, "cf0100000000ffffffffffffffff" },
    { 0x000000ffffffffffull, 0x0000000000000000ull, "c0ffffffffff" },
    { 0x000000ffffffffffull, 0x0000000000000001ull, "c1ffffffffff" },
    { 0x000000ffffffffffull, 0x00000000000000ffull, "c8ffffffffffff" },
    { 0x000000ffffffffffull, 0x0000000000000100ull, "c9ffffffffff0100" },
    { 0x000000ffffffffffull, 0x000000000000ffffull, "c9ffffffffffffff" },
    { 0x000000ffffffffffull, 0x0000000000010000ull, "caffffffffff010000" },
    { 0x000000ffffffffffull, 0x0000000000ffffffull, "caffffffffffffffff" },
    { 0x000000ffffffffffull, 0x0000000001000000ull, "cbffffffffff01000000" },
    { 0x000000ffffffffffull, 0x00000000ffffffffull, "cbffffffffffffffffff" },
    { 0x000000ffffffffffull, 0x0000000100000000ull, "ccffffffffff0100000000" },
    { 0x000000ffffffffffull, 0x000000ffffffffffull, "ccffffffffffffffffffff" },
    { 0x000000ffffffffffull, 0x0000010000000000ull, "cdffffffffff010000000000" },
    { 0x000000ffffffffffull, 0x0000ffffffffffffull, "cdffffffffffffffffffffff" },
    { 0x000000ffffffffffull, 0x0001000000000000ull, "ceffffffffff01000000000000" },
    { 0x000000ffffffffffull, 0x00ffffffffffffffull, "ceffffffffffffffffffffffff" },
    { 0x000000ffffffffffull, 0x0100000000000000ull, "cfffffffffff0100000000000000" },
    { 0x000000ffffffffffull, 0xffffffffffffffffull, "cfffffffffffffffffffffffffff" },
    { 0x0000010000000000ull, 0x0000000000000000ull, "d0010000000000" },
    { 0x0000010000000000ull, 0x0000000000000001ull, "d1010000000000" },
    { 0x0000010000000000ull, 0x00000000000000ffull, "d8010000000000ff" },
    { 0x0000010000000000ull, 0x0000000000000100ull, "d90100000000000100" },
    { 0x0000010000000000ull, 0x000000000000ffffull, "d9010000000000ffff" },
    { 0x0000010000000000ull, 0x0000000000010000ull, "da010000000000010000" },
    { 0x0000010000000000ull, 0x0000000000ffffffull, "da010000000000ffffff" },
    { 0x0000010000000000ull, 0x0000000001000000ull, "db01000000000001000000" },
    { 0x0000010000000000ull, 0x00000000ffffffffull, "db010000000000ffffffff" },
    { 0x0000010000000000ull, 0x0000000100000000ull, "dc0100000000000100000000" },
    { 0x0000010000000000ull, 0x000000ffffffffffull, "dc010000000000ffffffffff" },
    { 0x0000010000000000ull, 0x0000010000000000ull, "dd010000000000010000000000" },
    { 0x0000010000000000ull, 0x0000ffffffffffffull, "dd010000000000ffffffffffff" },
    { 0x0000010000000000ull, 0x0001000000000000ull, "de01000000000001000000000000" },
    { 0x0000010000000000ull, 0x00ffffffffffffffull, "de010000000000ffffffffffffff" },
    { 0x0000010000000000ull, 0x0100000000000000ull, "df0100000000000100000000000000" },
    { 0x0000010000000000ull, 0xffffffffffffffffull, "df010000000000ffffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0000000000000000ull, "d0ffffffffffff" },
    { 0x0000ffffffffffffull, 0x0000000000000001ull, "d1ffffffffffff" },
    { 0x0000ffffffffffffull, 0x00000000000000ffull, "d8ffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0000000000000100ull, "d9ffffffffffff0100" },
    { 0x0000ffffffffffffull, 0x000000000000ffffull, "d9ffffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0000000000010000ull, "daffffffffffff010000" },
    { 0x0000ffffffffffffull, 0x0000000000ffffffull, "daffffffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0000000001000000ull, "dbffffffffffff01000000" },
    { 0x0000ffffffffffffull, 0x00000000ffffffffull, "dbffffffffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0000000100000000ull, "dcffffffffffff0100000000" },
    { 0x0000ffffffffffffull, 0x000000ffffffffffull, "dcffffffffffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0000010000000000ull, "ddffffffffffff010000000000" },
    { 0x0000ffffffffffffull, 0x0000ffffffffffffull, "ddffffffffffffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0001000000000000ull, "deffffffffffff01000000000000" },
    { 0x0000ffffffffffffull, 0x00ffffffffffffffull, "deffffffffffffffffffffffffff" },
    { 0x0000ffffffffffffull, 0x0100000000000000ull, "dfffffffffffff0100000000000000" },
    { 0x0000ffffffffffffull, 0xffffffffffffffffull, "dfffffffffffffffffffffffffffff" },
    { 0x0001000000000000ull, 0x0000000000000000ull, "e001000000000000" },
    { 0x0001000000000000ull, 0x0000000000000001ull, "e101000000000000" },
    { 0x0001000000000000ull, 0x00000000000000ffull, "e801000000000000ff" },
    { 0x0001000000000000ull, 0x0000000000000100ull, "e9010000000000000100" },
    { 0x0001000000000000ull, 0x000000000000ffffull, "e901000000000000ffff" },
    { 0x0001000000000000ull, 0x0000000000010000ull, "ea01000000000000010000" },
    { 0x0001000000000000ull, 0x0000000000ffffffull, "ea01000000000000ffffff" },
    { 0x0001000000000000ull, 0x0000000001000000ull, "eb0100000000000001000000" },
    { 0x0001000000000000ull, 0x00000000ffffffffull, "eb01000000000000ffffffff" },
    { 0x0001000000000000ull, 0x0000000100000000ull, "ec010000000000000100000000" },
    { 0x0001000000000000ull, 0x000000ffffffffffull, "ec01000000000000ffffffffff" },
    { 0x0001000000000000ull, 0x0000010000000000ull, "ed01000000000000010000000000" },
    { 0x0001000000000000ull, 0x0000ffffffffffffull, "ed01000000000000ffffffffffff" },
    { 0x0001000000000000ull, 0x0001000000000000ull, "ee0100000000000001000000000000" },
    { 0x0001000000000000ull, 0x00ffffffffffffffull, "ee01000000000000ffffffffffffff" },
    { 0x0001000000000000ull, 0x0100000000000000ull, "ef010000000000000100000000000000" },
    { 0x0001000000000000ull, 0xffffffffffffffffull, "ef01000000000000ffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0000000000000000ull, "e0ffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0000000000000001ull, "e1ffffffffffffff" },
    { 0x00ffffffffffffffull, 0x00000000000000ffull, "e8ffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0000000000000100ull, "e9ffffffffffffff0100" },
    { 0x00ffffffffffffffull, 0x000000000000ffffull, "e9ffffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0000000000010000ull, "eaffffffffffffff010000" },
    { 0x00ffffffffffffffull, 0x0000000000ffffffull, "eaffffffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0000000001000000ull, "ebffffffffffffff01000000" },
    { 0x00ffffffffffffffull, 0x00000000ffffffffull, "ebffffffffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0000000100000000ull, "ecffffffffffffff0100000000" },
    { 0x00ffffffffffffffull, 0x000000ffffffffffull, "ecffffffffffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0000010000000000ull, "edffffffffffffff010000000000" },
    { 0x00ffffffffffffffull, 0x0000ffffffffffffull, "edffffffffffffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0001000000000000ull, "eeffffffffffffff01000000000000" },
    { 0x00ffffffffffffffull, 0x00ffffffffffffffull, "eeffffffffffffffffffffffffffff" },
    { 0x00ffffffffffffffull, 0x0100000000000000ull, "efffffffffffffff0100000000000000" },
    { 0x00ffffffffffffffull, 0xffffffffffffffffull, "efffffffffffffffffffffffffffffff" },
    { 0x0100000000000000ull, 0x0000000000000000ull, "f00100000000000000" },
    { 0x0100000000000000ull, 0x0000000000000001ull, "f10100000000000000" },
    { 0x0100000000000000ull, 0x00000000000000ffull, "f80100000000000000ff" },
    { 0x0100000000000000ull, 0x0000000000000100ull, "f901000000000000000100" },
    { 0x0100000000000000ull, 0x000000000000ffffull, "f90100000000000000ffff" },
    { 0x0100000000000000ull, 0x0000000000010000ull, "fa0100000000000000010000" },
    { 0x0100000000000000ull, 0x0000000000ffffffull, "fa0100000000000000ffffff" },
    { 0x0100000000000000ull, 0x0000000001000000ull, "fb010000000000000001000000" },
    { 0x0100000000000000ull, 0x00000000ffffffffull, "fb0100000000000000ffffffff" },
    { 0x0100000000000000ull, 0x0000000100000000ull, "fc01000000000000000100000000" },
    { 0x0100000000000000ull, 0x000000ffffffffffull, "fc0100000000000000ffffffffff" },
    { 0x0100000000000000ull, 0x0000010000000000ull, "fd0100000000000000010000000000" },
    { 0x0100000000000000ull, 0x0000ffffffffffffull, "fd0100000000000000ffffffffffff" },
    { 0x0100000000000000ull, 0x0001000000000000ull, "fe010000000000000001000000000000" },
    { 0x0100000000000000ull, 0x00ffffffffffffffull, "fe0100000000000000ffffffffffffff" },
    { 0x0100000000000000ull, 0x0100000000000000ull, "ff01000000000000000100000000000000" },
    { 0x0100000000000000ull, 0xffffffffffffffffull, "ff0100000000000000ffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0000000000000000ull, "f0ffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0000000000000001ull, "f1ffffffffffffffff" },
    { 0xffffffffffffffffull, 0x00000000000000ffull, "f8ffffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0000000000000100ull, "f9ffffffffffffffff0100" },
    { 0xffffffffffffffffull, 0x000000000000ffffull, "f9ffffffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0000000000010000ull, "faffffffffffffffff010000" },
    { 0xffffffffffffffffull, 0x0000000000ffffffull, "faffffffffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0000000001000000ull, "fbffffffffffffffff01000000" },
    { 0xffffffffffffffffull, 0x00000000ffffffffull, "fbffffffffffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0000000100000000ull, "fcffffffffffffffff0100000000" },
    { 0xffffffffffffffffull, 0x000000ffffffffffull, "fcffffffffffffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0000010000000000ull, "fdffffffffffffffff010000000000" },
    { 0xffffffffffffffffull, 0x0000ffffffffffffull, "fdffffffffffffffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0001000000000000ull, "feffffffffffffffff01000000000000" },
    { 0xffffffffffffffffull, 0x00ffffffffffffffull, "feffffffffffffffffffffffffffffff" },
    { 0xffffffffffffffffull, 0x0100000000000000ull, "ffffffffffffffffff0100000000000000" },
    { 0xffffffffffffffffull, 0xffffffffffffffffull, "ffffffffffffffffffffffffffffffffff" },
};

static void sframe_headers(void) {
    int bad = 0;
    for (size_t i = 0; i < sizeof HDRS / sizeof HDRS[0]; i++) {
        uint8_t want[OC_SFRAME_HDR_MAX], got[OC_SFRAME_HDR_MAX];
        size_t wl = unhex(HDRS[i].hdr, want, sizeof want);
        size_t gl = oc_sframe_header_encode(HDRS[i].kid, HDRS[i].ctr, got);
        uint64_t k = 0, c = 0; size_t hl = 0;
        if (gl != wl || memcmp(got, want, wl) != 0 ||
            oc_sframe_header_decode(want, wl, &k, &c, &hl) != 0 ||
            k != HDRS[i].kid || c != HDRS[i].ctr || hl != wl) bad++;
    }
    CHECK(bad == 0);
    /* Not minimal, so not accepted: 5 written in a byte of its own, and 0x100
     * written in three. And a header cut short. */
    uint64_t k, c; size_t hl;
    const uint8_t pad_small[] = { 0x08, 0x05 }, pad_long[] = { 0x0a, 0x00, 0x01, 0x00 }, cut[] = { 0x09, 0x01 };
    CHECK(oc_sframe_header_decode(pad_small, sizeof pad_small, &k, &c, &hl) != 0);
    CHECK(oc_sframe_header_decode(pad_long, sizeof pad_long, &k, &c, &hl) != 0);
    CHECK(oc_sframe_header_decode(cut, sizeof cut, &k, &c, &hl) != 0);
}

/* RFC 9605 Appendix C.3, cipher_suite 0x0004. */
static void sframe_vector(void) {
    uint8_t base[16], meta[32], pt[64], want[128], got[128], back[64];
    size_t bl = unhex("000102030405060708090a0b0c0d0e0f", base, sizeof base);
    size_t ml = unhex("4945544620534672616d65205747", meta, sizeof meta);
    size_t pl = unhex("64726166742d696574662d736672616d652d656e63", pt, sizeof pt);
    size_t wl = unhex("9901234567b7412c2513a1b66dbb48841bbaf17f598751176ad847681a69c6d0b091c07018ce4adb34eb",
                      want, sizeof want);
    uint8_t key[16], salt[12];
    unhex("d34f547f4ca4f9a7447006fe7fcbf768", key, sizeof key);
    unhex("75234edefe07819026751816", salt, sizeof salt);

    oc_sframe_key k;
    CHECK(oc_sframe_key_init(&k, 0x123, base, bl) == 0);
    CHECK(memcmp(k.key, key, 16) == 0 && memcmp(k.salt, salt, 12) == 0);
    size_t gl = 0;
    CHECK(oc_sframe_encrypt(&k, 0x4567, meta, ml, pt, pl, got, sizeof got, &gl) == 0);
    CHECK(gl == wl && memcmp(got, want, wl) == 0);
    size_t n = 0; uint64_t ctr = 0;
    CHECK(oc_sframe_decrypt(&k, meta, ml, want, wl, back, sizeof back, &n, &ctr) == 0);
    CHECK(n == pl && memcmp(back, pt, pl) == 0 && ctr == 0x4567);

    /* Anything altered is refused: a byte of the header (the AAD), of the body,
     * of the tag; other metadata; a truncation. */
    int refused = 1;
    size_t spots[] = { 0, 3, 6, wl / 2, wl - 1 };
    for (size_t i = 0; i < sizeof spots / sizeof spots[0]; i++) {
        memcpy(got, want, wl);
        got[spots[i]] ^= 0x01;
        if (oc_sframe_decrypt(&k, meta, ml, got, wl, back, sizeof back, &n, &ctr) == 0) refused = 0;
    }
    CHECK(refused);
    CHECK(oc_sframe_decrypt(&k, meta, ml - 1, want, wl, back, sizeof back, &n, &ctr) != 0);
    CHECK(oc_sframe_decrypt(&k, meta, ml, want, wl - 1, back, sizeof back, &n, &ctr) != 0);

    /* Another key for the same KID (another sender's, another epoch's) opens
     * nothing, and a key for another KID is not even tried. */
    oc_sframe_key other, elsewhere;
    base[0] ^= 1;
    CHECK(oc_sframe_key_init(&other, 0x123, base, bl) == 0);
    CHECK(oc_sframe_decrypt(&other, meta, ml, want, wl, back, sizeof back, &n, &ctr) != 0);
    CHECK(oc_sframe_key_init(&elsewhere, 0x124, base, bl) == 0);
    CHECK(oc_sframe_decrypt(&elsewhere, meta, ml, want, wl, back, sizeof back, &n, &ctr) != 0);

    oc_sframe_key_wipe(&k);
    uint8_t zero[sizeof k];
    memset(zero, 0, sizeof zero);
    CHECK(memcmp(&k, zero, sizeof k) == 0);
    CHECK(oc_sframe_encrypt(&k, 1, NULL, 0, pt, pl, got, sizeof got, &gl) != 0);
}

static void sframe_replay(void) {
    oc_sframe_replay w;
    memset(&w, 0, sizeof w);
    CHECK(oc_sframe_replay_ok(&w, 0));
    oc_sframe_replay_mark(&w, 0);
    CHECK(!oc_sframe_replay_ok(&w, 0));               /* a replay */
    CHECK(oc_sframe_replay_ok(&w, 5));
    oc_sframe_replay_mark(&w, 5);
    CHECK(oc_sframe_replay_ok(&w, 3));                /* late, within the window */
    oc_sframe_replay_mark(&w, 3);
    CHECK(!oc_sframe_replay_ok(&w, 3));
    CHECK(oc_sframe_replay_ok(&w, 4));
    oc_sframe_replay_mark(&w, 2000);
    CHECK(!oc_sframe_replay_ok(&w, 2000 - OC_SFRAME_REPLAY_WINDOW));   /* fell out */
    CHECK(oc_sframe_replay_ok(&w, 2000 - OC_SFRAME_REPLAY_WINDOW + 1));
    CHECK(!oc_sframe_replay_ok(&w, 5));
    /* The slot counter 5 used to hold (5 + 2 * 1024 = 2053) is fresh, not seen. */
    CHECK(oc_sframe_replay_ok(&w, 2053));
    oc_sframe_replay_mark(&w, 2001);
    CHECK(oc_sframe_replay_ok(&w, 1999));
    CHECK(!oc_sframe_replay_ok(&w, 2001));
}

/* RFC 9180 Appendix A.1.3: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, AES-128-GCM,
 * Auth mode, the encryption at sequence number 0. */
static void hpke_vector(void) {
    uint8_t info[64], skE[32], pkE[32], skR[32], pkR[32], skS[32], pkS[32];
    uint8_t pt[64], aad[16], want[96], enc[32], ct[96], back[64], pk[32];
    size_t il = unhex("4f6465206f6e2061204772656369616e2055726e", info, sizeof info);
    unhex("ff4442ef24fbc3c1ff86375b0be1e77e88a0de1e79b30896d73411c5ff4c3518", skE, 32);
    unhex("23fb952571a14a25e3d678140cd0e5eb47a0961bb18afcf85896e5453c312e76", pkE, 32);
    unhex("fdea67cf831f1ca98d8e27b1f6abeb5b7745e9d35348b80fa407ff6958f9137e", skR, 32);
    unhex("1632d5c2f71c2b38d0a8fcc359355200caa8b1ffdf28618080466c909cb69b2e", pkR, 32);
    unhex("dc4a146313cce60a278a5323d321f051c5707e9c45ba21a3479fecdf76fc69dd", skS, 32);
    unhex("8b0c70873dc5aecb7f9ee4e62406a397b350e57012be45cf53b7105ae731790b", pkS, 32);
    size_t pl = unhex("4265617574792069732074727574682c20747275746820626561757479", pt, sizeof pt);
    size_t al = unhex("436f756e742d30", aad, sizeof aad);
    size_t wl = unhex("5fd92cc9d46dbf8943e72a07e42f363ed5f721212cd90bcfd072bfd9f44e06b80fd17824947496e21b680c141b",
                      want, sizeof want);

    CHECK(oc_x25519_public(skE, pk) == 0 && memcmp(pk, pkE, 32) == 0);
    CHECK(oc_x25519_public(skR, pk) == 0 && memcmp(pk, pkR, 32) == 0);
    CHECK(oc_x25519_public(skS, pk) == 0 && memcmp(pk, pkS, 32) == 0);

    CHECK(wl == pl + OC_HPKE_TAG_LEN);
    CHECK(oc_hpke_seal_auth_with(skE, pkR, skS, info, il, aad, al, pt, pl, enc, ct) == 0);
    CHECK(memcmp(enc, pkE, 32) == 0);
    CHECK(memcmp(ct, want, wl) == 0);
    CHECK(oc_hpke_open_auth(pkE, skR, pkS, info, il, aad, al, want, wl, back) == 0);
    CHECK(memcmp(back, pt, pl) == 0);
}

/* Beyond the vector: a fresh seal opens; only the right receiver opens it; only
 * with the right sender's key; only for the same info and aad; not altered. */
static void hpke_properties(void) {
    uint8_t skA[32], pkA[32], skB[32], pkB[32], skC[32], pkC[32];
    CHECK(oc_x25519_keypair(skA, pkA) == 0);
    CHECK(oc_x25519_keypair(skB, pkB) == 0);
    CHECK(oc_x25519_keypair(skC, pkC) == 0);
    CHECK(memcmp(pkA, pkB, 32) != 0 && memcmp(skA, skB, 32) != 0);

    const uint8_t info[] = "OpenChime call key v1 test";
    uint8_t base[16], enc[32], ct[16 + OC_HPKE_TAG_LEN], back[16];
    CHECK(oc_e2e_random(base, sizeof base) == 0);
    CHECK(oc_hpke_seal_auth(pkB, skA, info, sizeof info, NULL, 0, base, 16, enc, ct) == 0);
    CHECK(oc_hpke_open_auth(enc, skB, pkA, info, sizeof info, NULL, 0, ct, sizeof ct, back) == 0);
    CHECK(memcmp(back, base, 16) == 0);

    /* C is not the receiver; C is not the sender; another info (another epoch,
     * call, sender or recipient); other aad; a flipped bit in enc or ct. */
    CHECK(oc_hpke_open_auth(enc, skC, pkA, info, sizeof info, NULL, 0, ct, sizeof ct, back) != 0);
    CHECK(oc_hpke_open_auth(enc, skB, pkC, info, sizeof info, NULL, 0, ct, sizeof ct, back) != 0);
    uint8_t info2[sizeof info];
    memcpy(info2, info, sizeof info);
    info2[sizeof info - 2] ^= 1;
    CHECK(oc_hpke_open_auth(enc, skB, pkA, info2, sizeof info2, NULL, 0, ct, sizeof ct, back) != 0);
    const uint8_t aad[] = { 1 };
    CHECK(oc_hpke_open_auth(enc, skB, pkA, info, sizeof info, aad, 1, ct, sizeof ct, back) != 0);
    uint8_t bad[sizeof ct];
    memcpy(bad, ct, sizeof ct); bad[3] ^= 0x80;
    CHECK(oc_hpke_open_auth(enc, skB, pkA, info, sizeof info, NULL, 0, bad, sizeof bad, back) != 0);
    uint8_t bad_enc[32];
    memcpy(bad_enc, enc, 32); bad_enc[7] ^= 0x10;
    CHECK(oc_hpke_open_auth(bad_enc, skB, pkA, info, sizeof info, NULL, 0, ct, sizeof ct, back) != 0);
    uint8_t zero[16] = { 0 };
    CHECK(memcmp(back, zero, 16) == 0);                  /* a failure leaves nothing */

    /* A low-order public key (the all-zero point) is refused, not turned into a
     * shared secret of zeros. */
    uint8_t lowpk[32] = { 0 };
    CHECK(oc_hpke_seal_auth(lowpk, skA, info, sizeof info, NULL, 0, base, 16, enc, ct) != 0);

    /* Two seals of the same key to the same receiver differ: a fresh ephemeral
     * each time. */
    uint8_t enc2[32], ct2[sizeof ct];
    CHECK(oc_hpke_seal_auth(pkB, skA, info, sizeof info, NULL, 0, base, 16, enc2, ct2) == 0);
    CHECK(memcmp(enc, enc2, 32) != 0 && memcmp(ct, ct2, sizeof ct) != 0);
}

int run_e2e_tests(void) {
    printf("test_e2e: SFrame headers and suite 0x0004 against RFC 9605, tampering, wrong keys, the replay window; "
           "HPKE Auth mode against RFC 9180 A.1.3, wrong receiver, wrong sender, info and aad binding, low-order keys\n");
    sframe_headers();
    sframe_vector();
    sframe_replay();
    hpke_vector();
    hpke_properties();
    return failures;
}
