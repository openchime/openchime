/* The palette's contrast guarantee (REQ-262, ARCH-82).
 *
 * The audit (scripts/gui_audit.sh) grades whatever ink happens to land on
 * whatever surface in a captured frame. That is a useful check and it is not a
 * guarantee: it is empirical, so a pair no scene renders is never graded; it
 * reads text ops only; and it runs on a Windows box against one colour scheme
 * when somebody remembers. Both defects this file exists for were live while it
 * passed — no scene leaves a draft, so the pencil was never drawn, and the
 * scenes that show a composer either type into it or were run under Midnight.
 *
 * So the guarantee is asserted here instead, over the whole product of ink,
 * surface, mode and scheme, from the palette itself. No frame is captured and
 * nothing has to be rendered for it to hold. */

#include "check.h"
#include "theme.h"

#include <stdio.h>

/* The surfaces a laddered ink is actually painted on. Not every token: RAIL
 * carries its own foreground (TH_RAIL_ICON) by design, and BORDER is a stroke
 * nothing is written on. Adding a surface here is how a new one gets covered. */
static const struct { int tok; const char *name; } SURFACES[] = {
    { TH_BASE,    "BASE"    },
    { TH_SIDEBAR, "SIDEBAR" },
    { TH_HEADER,  "HEADER"  },
    { TH_INPUT,   "INPUT"   },
    { TH_SELECT,  "SELECT"  },
    { TH_HOVER,   "HOVER"   },
};
#define N_SURFACES ((int)(sizeof SURFACES / sizeof SURFACES[0]))

static const struct { int tok; const char *name; } INKS[] = {
    { TH_FAINT, "FAINT" },
    { TH_MUTED, "MUTED" },
    { TH_TEXT,  "TEXT"  },
};
#define N_INKS ((int)(sizeof INKS / sizeof INKS[0]))

/* The ratio the repo's own audit fails below, and the one WCAG asks of non-text
 * UI. Stated as a number here rather than borrowed, so that if OC_CONTRAST_FLOOR
 * is ever lowered to make a failure go away, this file disagrees out loud. */
#define FLOOR 3.0f

static void test_every_pair_clears_the_floor(void) {
    int checked = 0, failures_here = 0;
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
        for (int light = 0; light < 2; light++) {
            oc_theme_set_scheme(scheme);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            for (int i = 0; i < N_INKS; i++) {
                for (int s = 0; s < N_SURFACES; s++) {
                    uint32_t surf = oc_theme[SURFACES[s].tok];
                    uint32_t ink  = oc_ink_on(INKS[i].tok, SURFACES[s].tok);
                    float c = oc_theme_contrast(ink, surf);
                    checked++;
                    if (c < FLOOR) {
                        failures_here++;
                        printf("  contrast: %s ink on %s, %s %s: %06X on %06X is "
                               "%.2f:1, under %.0f:1\n",
                               INKS[i].name, SURFACES[s].name,
                               oc_theme_scheme_name(scheme), light ? "light" : "dark",
                               ink, surf, (double)c, (double)FLOOR);
                    }
                }
            }
        }
    }
    /* 3 inks x 6 surfaces x 4 schemes x 2 modes. */
    CHECK(checked == N_INKS * N_SURFACES * OC_SCHEME_COUNT * 2);
    CHECK(checked == 144);
    CHECK(failures_here == 0);
}

/* An ink may be strengthened to stay legible, never weakened to stay pretty:
 * the resolver walks the ladder forward from the rung asked for. Without this a
 * resolver that returned the FIRST clearing rung — rather than the first at or
 * after the one requested — would answer TEXT's question with FAINT and pass
 * the floor test above while quietly flattening the hierarchy. */
static void test_escalation_is_one_way(void) {
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
        for (int light = 0; light < 2; light++) {
            oc_theme_set_scheme(scheme);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            for (int s = 0; s < N_SURFACES; s++) {
                int tok = SURFACES[s].tok;
                /* TEXT is the top rung and can only ever answer itself. */
                CHECK(oc_ink_on(TH_TEXT, tok) == oc_theme[TH_TEXT]);
                /* MUTED answers itself or TEXT, never FAINT. */
                uint32_t m = oc_ink_on(TH_MUTED, tok);
                CHECK(m == oc_theme[TH_MUTED] || m == oc_theme[TH_TEXT]);
                CHECK(m != oc_theme[TH_FAINT] ||
                      oc_theme[TH_FAINT] == oc_theme[TH_MUTED]);
            }
        }
    }
}

/* The hierarchy survives where it is legible. A resolver that escalated
 * everything would pass both tests above and leave the app with one ink, so
 * something has to assert that FAINT is still FAINT where FAINT reads. */
static void test_the_faint_tier_still_exists(void) {
    oc_theme_set_scheme(OC_SCHEME_MIDNIGHT);
    for (int light = 0; light < 2; light++) {
        oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
        /* The canvas and the sidebar are what the faint tier was chosen
         * against, and it clears there (3.20-3.47:1). */
        CHECK(oc_ink_on(TH_FAINT, TH_BASE)    == oc_theme[TH_FAINT]);
        CHECK(oc_ink_on(TH_FAINT, TH_SIDEBAR) == oc_theme[TH_FAINT]);
        /* And it is genuinely a step below the tier above it, or the ladder is
         * decoration. */
        CHECK(oc_theme[TH_FAINT] != oc_theme[TH_MUTED]);
        CHECK(oc_theme_contrast(oc_theme[TH_FAINT], oc_theme[TH_BASE]) <
              oc_theme_contrast(oc_theme[TH_MUTED], oc_theme[TH_BASE]));
    }
}

/* The two pairs the defect was reported as, named so a regression says which
 * thing on screen went wrong rather than only which numbers moved. */
static void test_the_reported_pairs(void) {
    /* The composer cue, dark: was FAINT on INPUT at 2.73:1. */
    oc_theme_set_scheme(OC_SCHEME_MIDNIGHT);
    oc_theme_apply(OC_THEME_DARK);
    CHECK(oc_theme_contrast(oc_theme[TH_FAINT], oc_theme[TH_INPUT]) < FLOOR);
    CHECK(oc_theme_contrast(oc_ink_on(TH_FAINT, TH_INPUT), oc_theme[TH_INPUT]) >= FLOOR);

    /* The draft marker on a selected row. Worst in the app, and worst of all
     * under Teal (1.58:1) — a scheme the audit never runs. */
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
        for (int light = 0; light < 2; light++) {
            oc_theme_set_scheme(scheme);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            CHECK(oc_theme_contrast(oc_theme[TH_FAINT], oc_theme[TH_SELECT]) < FLOOR);
            CHECK(oc_theme_contrast(oc_ink_on(TH_FAINT, TH_SELECT),
                                    oc_theme[TH_SELECT]) >= FLOOR);
        }
    }
}

/* The luminance and ratio helpers, against values with known answers — the
 * arithmetic everything above rests on. */
static void test_contrast_arithmetic(void) {
    CHECK(oc_theme_contrast(0xFFFFFF, 0x000000) > 20.99f);   /* 21:1 exactly */
    CHECK(oc_theme_contrast(0xFFFFFF, 0x000000) < 21.01f);
    CHECK(oc_theme_contrast(0x808080, 0x808080) > 0.99f);    /* 1:1 with itself */
    CHECK(oc_theme_contrast(0x808080, 0x808080) < 1.01f);
    /* Symmetric: the order of the arguments is not part of the answer. */
    CHECK(oc_theme_contrast(0x1E2E4C, 0x93A1BC) ==
          oc_theme_contrast(0x93A1BC, 0x1E2E4C));
    CHECK(oc_theme_luminance(0x000000) < 0.0001f);
    CHECK(oc_theme_luminance(0xFFFFFF) > 0.9999f);
}

int run_theme_tests(void) {
    printf("test_theme: the contrast floor over every ink x surface x scheme x mode, "
           "one-way escalation, the faint tier survives, the reported pairs, "
           "the ratio arithmetic\n");
    test_contrast_arithmetic();
    test_every_pair_clears_the_floor();
    test_escalation_is_one_way();
    test_the_faint_tier_still_exists();
    test_the_reported_pairs();
    /* Leave the palette as the rest of the suite expects to find it. */
    oc_theme_set_scheme(OC_SCHEME_MIDNIGHT);
    oc_theme_apply(OC_THEME_DARK);
    return failures;
}
