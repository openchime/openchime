/* The interface font (ARCH-97, ARCH-108): the list on offer, how a choice
 * resolves against what the system has, and the pinned line box and baseline
 * derived from a family's own figures.
 *
 * Resolution is asserted with a faked presence function, so the fallback is
 * exercised on a machine that has every font as well as on one that has none;
 * the pin is pure arithmetic, asserted against Segoe UI's own figures, which
 * must give exactly the pairs ARCH-97's table states. */

#include "check.h"
#include "typeface.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* A system that has exactly the families named in `user` (NULL-terminated). */
static int fake_present(void *user, const char *family) {
    for (const char *const *f = user; *f; f++)
        if (!strcmp(*f, family)) return 1;
    return 0;
}

static void test_the_list(void) {
    CHECK(OC_TYPEFACE_COUNT == 4);
    CHECK(OC_TYPEFACE_DEFAULT == 0);
    CHECK(!strncmp(OC_TYPEFACES[0].label, "Default", 7));
    CHECK(!strcmp(OC_TYPEFACES[0].family, "Segoe UI"));
    CHECK(!strcmp(OC_TYPEFACES[1].family, "Verdana"));
    CHECK(!strcmp(OC_TYPEFACES[2].family, "Calibri"));
    CHECK(!strcmp(OC_TYPEFACES[3].family, "Sitka Text"));
}

static void test_resolution(void) {
    const char *all[] = { "Segoe UI Variable Text", "Segoe UI", "Verdana", "Calibri", "Sitka Text", NULL };
    const char *old[] = { "Segoe UI", NULL };
    int fb = -1;
    /* Present: the family itself, no fallback. */
    CHECK(!strcmp(oc_typeface_resolve(OC_TYPEFACE_VERDANA, fake_present, all, &fb), "Verdana") && fb == 0);
    CHECK(!strcmp(oc_typeface_resolve(OC_TYPEFACE_SITKA, fake_present, all, &fb), "Sitka Text") && fb == 0);
    /* Default is the platform's UI family, whichever of the two it has. */
    CHECK(!strcmp(oc_typeface_resolve(OC_TYPEFACE_DEFAULT, fake_present, all, &fb), "Segoe UI Variable Text") && fb == 0);
    CHECK(!strcmp(oc_typeface_resolve(OC_TYPEFACE_DEFAULT, fake_present, old, &fb), "Segoe UI") && fb == 0);
    /* Missing: the default, and the flag. */
    CHECK(!strcmp(oc_typeface_resolve(OC_TYPEFACE_CALIBRI, fake_present, old, &fb), "Segoe UI") && fb == 1);
    CHECK(!strcmp(oc_typeface_resolve_family("No Such Family", fake_present, all, &fb), "Segoe UI Variable Text") && fb == 1);
    /* Out of range is Default. */
    CHECK(!strcmp(oc_typeface_resolve(-1, fake_present, all, &fb), "Segoe UI Variable Text") && fb == 0);
    CHECK(!strcmp(oc_typeface_resolve(OC_TYPEFACE_COUNT, fake_present, old, &fb), "Segoe UI") && fb == 0);
}

static void test_segoe_gives_the_table(void) {
    static const struct { float size, lh, bl; } T[] = {
        { 17.0f, 26, 19 },   /* display */
        { 15.0f, 23, 17 },   /* title */
        { 14.0f, 22, 16 },   /* ui */
        { 12.5f, 19, 14 },   /* meta */
        { 10.0f, 15, 11 },   /* micro */
    };
    const float asc = 2210.0f / 2048.0f, cap = 1434.0f / 2048.0f;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        float lh = 0, bl = 0;
        oc_typeface_pin(T[i].size, asc, cap, &lh, &bl);
        if (lh != T[i].lh || bl != T[i].bl)
            printf("  size %.1f: %g/%g, want %g/%g\n", (double)T[i].size, (double)lh, (double)bl,
                   (double)T[i].lh, (double)T[i].bl);
        CHECK(lh == T[i].lh && bl == T[i].bl);
        /* Unreadable figures fall back to Segoe UI's: the same pairs. */
        float lh2 = 0, bl2 = 0;
        oc_typeface_pin(T[i].size, 0.0f, 0.0f, &lh2, &bl2);
        CHECK(lh2 == lh && bl2 == bl);
    }
}

static void test_other_figures_hold_the_ascent(void) {
    /* Verdana-, Calibri- and Sitka-like figures, and a range of sizes and text
     * scales: the baseline is a whole DIP that holds the ascent, and the box
     * holds the baseline. */
    static const float FIG[][2] = { { 1.005f, 0.727f }, { 0.750f, 0.644f }, { 0.920f, 0.660f }, { 1.20f, 0.75f } };
    static const float SIZE[] = { 10.0f, 12.5f, 14.0f, 15.0f, 17.0f };
    static const float K[] = { 0.90f, 1.00f, 1.12f, 1.25f };
    for (size_t f = 0; f < sizeof FIG / sizeof FIG[0]; f++)
        for (size_t s = 0; s < sizeof SIZE / sizeof SIZE[0]; s++)
            for (size_t k = 0; k < sizeof K / sizeof K[0]; k++) {
                float size = SIZE[s] * K[k], lh = 0, bl = 0;
                oc_typeface_pin(size, FIG[f][0], FIG[f][1], &lh, &bl);
                CHECK(bl == floorf(bl) && lh == floorf(lh));
                CHECK(bl >= FIG[f][0] * size - 0.02f);
                CHECK(lh > bl);
            }
}

int run_typeface_tests(void) {
    printf("test_typeface: the list, resolution with a faked system, Segoe UI's pairs, "
           "the pin for other figures\n");
    test_the_list();
    test_resolution();
    test_segoe_gives_the_table();
    test_other_figures_hold_the_ascent();
    return failures;
}
