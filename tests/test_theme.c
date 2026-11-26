/* The palette's contrast guarantee (REQ-262, ARCH-82).
 *
 * Grading whatever ink happens to land on whatever surface in a captured frame
 * is not a guarantee: it is empirical, so a pair no scene renders is never
 * graded. Both defects this file exists for were of that kind -- no scene left
 * a draft, so the pencil was never drawn, and the scenes that showed a composer
 * either typed into it or ran under Midnight.
 *
 * So the guarantee is asserted here instead, over the whole product of ink,
 * surface, mode and scheme, from the palette itself. No frame is captured and
 * nothing has to be rendered for it to hold. */

#include "check.h"
#include "theme.h"
#include "protocol.h"   /* OC_PRESENCE_* */

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

/* Presence is never told by colour alone (REQ-122): each state is a shape of
 * its own, so a reader who cannot tell the green from the amber still can tell
 * online from away. */
static void test_presence_marks_are_distinct(void) {
    int on   = oc_presence_mark(OC_PRESENCE_ONLINE,  0);
    int away = oc_presence_mark(OC_PRESENCE_AWAY,    0);
    int off  = oc_presence_mark(OC_PRESENCE_OFFLINE, 0);
    int dnd  = oc_presence_mark(OC_PRESENCE_ONLINE,  1);
    CHECK(on == OC_PMARK_ONLINE && away == OC_PMARK_AWAY &&
          off == OC_PMARK_OFFLINE && dnd == OC_PMARK_DND);
    /* Four states, four shapes: no two share one. */
    int m[4] = { on, away, off, dnd };
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            CHECK(m[i] != m[j]);
    /* The pair the colours alone could not separate. */
    CHECK(on != away);
    /* Do-not-disturb is its own shape whatever the presence under it, and a
     * presence value the client does not know is offline, not online. */
    CHECK(oc_presence_mark(OC_PRESENCE_AWAY, 1)    == OC_PMARK_DND);
    CHECK(oc_presence_mark(OC_PRESENCE_OFFLINE, 1) == OC_PMARK_DND);
    CHECK(oc_presence_mark(7, 0) == OC_PMARK_OFFLINE);
}

/* The away mark is a ring and a half fill in TH_AWAY, so its outline has to
 * read on every surface a presence dot sits on, in every scheme and mode. The
 * dot's own surface ring separates it from the tile under it; this is the
 * colour against the row, the field and the canvas around it. */
static void test_away_ring_clears_the_floor(void) {
    int checked = 0;
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
        for (int light = 0; light < 2; light++) {
            oc_theme_set_scheme(scheme);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            for (int s = 0; s < N_SURFACES; s++) {
                float c = oc_theme_contrast(oc_theme[TH_AWAY], oc_theme[SURFACES[s].tok]);
                checked++;
                if (c < FLOOR)
                    printf("  contrast: AWAY ring on %s, %s %s: %06X on %06X is "
                           "%.2f:1, under %.0f:1\n",
                           SURFACES[s].name, oc_theme_scheme_name(scheme),
                           light ? "light" : "dark", oc_theme[TH_AWAY],
                           oc_theme[SURFACES[s].tok], (double)c, (double)FLOOR);
                CHECK(c >= FLOOR);
            }
        }
    }
    /* 6 surfaces x 4 schemes x 2 modes. */
    CHECK(checked == N_SURFACES * OC_SCHEME_COUNT * 2);
}

/* --- high contrast (REQ-262) ------------------------------------------------
 *
 * The high-contrast palettes promise more than the floor: 7:1 for every ink
 * text is written in, 4.5:1 for every ink a control is drawn in, each against
 * every surface it lands on. Asserted over both palettes under every scheme,
 * so a scheme that leaked into the high-contrast palette would fail here too. */

#define HC_TEXT     7.0f
#define HC_CONTROLS 4.5f

static const struct { int tok; const char *name; } HC_TEXT_INKS[] = {
    { TH_TEXT, "TEXT" }, { TH_MUTED, "MUTED" }, { TH_FAINT, "FAINT" },
    { TH_ACCENT, "ACCENT" }, { TH_DANGER, "DANGER" },
};
static const struct { int tok; const char *name; } HC_CONTROL_INKS[] = {
    { TH_BORDER, "BORDER" }, { TH_ACCENT_DIM, "ACCENT_DIM" },
    { TH_ONLINE, "ONLINE" }, { TH_AWAY, "AWAY" }, { TH_NOTICE, "NOTICE" },
};
/* The fills a label is drawn ON in TH_ON_ACCENT. */
static const struct { int tok; const char *name; } ACCENT_FILLS[] = {
    { TH_ACCENT, "ACCENT" }, { TH_ACCENT_DIM, "ACCENT_DIM" }, { TH_DANGER, "DANGER" },
};
#define NELEM(a) ((int)(sizeof (a) / sizeof (a)[0]))

/* One pair against a ratio: counted, and printed by name when it falls short. */
static int hc_pair(const char *ink, int ink_tok, const char *surf, int surf_tok,
                   int scheme, int light, float want) {
    float c = oc_theme_contrast(oc_theme[ink_tok], oc_theme[surf_tok]);
    if (c >= want) return 0;
    printf("  high contrast: %s on %s, %s %s: %06X on %06X is %.2f:1, under %.1f:1\n",
           ink, surf, oc_theme_scheme_name(scheme), light ? "light" : "dark",
           oc_theme[ink_tok], oc_theme[surf_tok], (double)c, (double)want);
    return 1;
}

static void test_high_contrast_text_clears_7(void) {
    int checked = 0, failures_here = 0;
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
        for (int light = 0; light < 2; light++) {
            oc_theme_set_scheme(scheme);
            oc_theme_set_high_contrast(1);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            for (int i = 0; i < NELEM(HC_TEXT_INKS); i++)
                for (int s = 0; s < N_SURFACES; s++, checked++)
                    failures_here += hc_pair(HC_TEXT_INKS[i].name, HC_TEXT_INKS[i].tok,
                                             SURFACES[s].name, SURFACES[s].tok,
                                             scheme, light, HC_TEXT);
            failures_here += hc_pair("RAIL_ICON", TH_RAIL_ICON, "RAIL", TH_RAIL,
                                     scheme, light, HC_TEXT);
            checked++;
            for (int f = 0; f < NELEM(ACCENT_FILLS); f++, checked++)
                failures_here += hc_pair("ON_ACCENT", TH_ON_ACCENT,
                                         ACCENT_FILLS[f].name, ACCENT_FILLS[f].tok,
                                         scheme, light, HC_TEXT);
            oc_theme_set_high_contrast(0);
        }
    }
    /* (5 inks x 6 surfaces + the rail + 3 fills) x 4 schemes x 2 modes. */
    CHECK(checked == (NELEM(HC_TEXT_INKS) * N_SURFACES + 1 + NELEM(ACCENT_FILLS)) *
                     OC_SCHEME_COUNT * 2);
    CHECK(checked == 272);
    CHECK(failures_here == 0);
}

static void test_high_contrast_controls_clear_4_5(void) {
    int checked = 0, failures_here = 0;
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
        for (int light = 0; light < 2; light++) {
            oc_theme_set_scheme(scheme);
            oc_theme_set_high_contrast(1);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            for (int i = 0; i < NELEM(HC_CONTROL_INKS); i++)
                for (int s = 0; s < N_SURFACES; s++, checked++)
                    failures_here += hc_pair(HC_CONTROL_INKS[i].name, HC_CONTROL_INKS[i].tok,
                                             SURFACES[s].name, SURFACES[s].tok,
                                             scheme, light, HC_CONTROLS);
            oc_theme_set_high_contrast(0);
        }
    }
    /* 5 inks x 6 surfaces x 4 schemes x 2 modes. */
    CHECK(checked == NELEM(HC_CONTROL_INKS) * N_SURFACES * OC_SCHEME_COUNT * 2);
    CHECK(checked == 240);
    CHECK(failures_here == 0);
}

/* With high contrast on, the scheme changes nothing: not the rail, not the
 * accent, not the selection derived from it -- not one token. */
static void test_high_contrast_ignores_the_scheme(void) {
    for (int light = 0; light < 2; light++) {
        uint32_t first[TH_COUNT];
        oc_theme_set_high_contrast(1);
        for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
            oc_theme_set_scheme(scheme);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            CHECK(oc_theme_high_contrast() == 1);
            for (int t = 0; t < TH_COUNT; t++) {
                if (scheme == 0) first[t] = oc_theme[t];
                else if (oc_theme[t] != first[t]) {
                    printf("  high contrast: token %d moved under %s %s: %06X, not %06X\n",
                           t, oc_theme_scheme_name(scheme), light ? "light" : "dark",
                           oc_theme[t], first[t]);
                    CHECK(oc_theme[t] == first[t]);
                }
            }
        }
        /* ...and it is not simply the ordinary palette: dark and light differ
         * from what they are without it. */
        oc_theme_set_high_contrast(0);
        int differ = 0;
        for (int t = 0; t < TH_COUNT; t++) differ += oc_theme[t] != first[t];
        CHECK(differ > 0);
    }
}

/* Off again gives back exactly what the mode and the scheme give without it. */
static void test_high_contrast_off_is_unchanged(void) {
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++) {
        for (int light = 0; light < 2; light++) {
            uint32_t before[TH_COUNT];
            oc_theme_set_high_contrast(0);
            oc_theme_set_scheme(scheme);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            for (int t = 0; t < TH_COUNT; t++) before[t] = oc_theme[t];
            oc_theme_set_high_contrast(1);
            CHECK(oc_theme_high_contrast() == 1);
            oc_theme_set_high_contrast(0);
            CHECK(oc_theme_high_contrast() == 0);
            for (int t = 0; t < TH_COUNT; t++) CHECK(oc_theme[t] == before[t]);
            /* The resolved inks come back too. */
            CHECK(oc_ink_on(TH_FAINT, TH_SELECT) != oc_theme[TH_FAINT]);
        }
    }
}

/* Without high contrast the ink on an accent is white, as it was a literal. */
static void test_on_accent_is_white_without_high_contrast(void) {
    oc_theme_set_high_contrast(0);
    for (int scheme = 0; scheme < OC_SCHEME_COUNT; scheme++)
        for (int light = 0; light < 2; light++) {
            oc_theme_set_scheme(scheme);
            oc_theme_apply(light ? OC_THEME_LIGHT : OC_THEME_DARK);
            CHECK(oc_theme[TH_ON_ACCENT] == 0xFFFFFF);
            CHECK(OC_COL_ON_ACCENT == 0xFFFFFFu);
        }
}

/* Off, On and Follow system, against a system that says on, off and nothing. */
static void test_high_contrast_follows_the_preference(void) {
    for (int sys = -1; sys <= 1; sys++) {
        CHECK(oc_high_contrast_on(OC_HC_ON, sys) == 1);
        CHECK(oc_high_contrast_on(OC_HC_OFF, sys) == 0);
        CHECK(oc_high_contrast_on(OC_HC_SYSTEM, sys) == (sys == 1));
        /* Outside the three reads as Follow system. */
        CHECK(oc_high_contrast_on(7, sys) == (sys == 1));
        CHECK(oc_high_contrast_on(-1, sys) == (sys == 1));
    }
}

int run_theme_tests(void) {
    printf("test_theme: the contrast floor over every ink x surface x scheme x mode, "
           "one-way escalation, the faint tier survives, the reported pairs, "
           "the ratio arithmetic, distinct presence marks, the away ring, "
           "high contrast at 7:1 and 4.5:1, blind to the scheme, off unchanged, "
           "white on the accent without it, and its preference\n");
    test_contrast_arithmetic();
    test_every_pair_clears_the_floor();
    test_escalation_is_one_way();
    test_the_faint_tier_still_exists();
    test_the_reported_pairs();
    test_presence_marks_are_distinct();
    test_away_ring_clears_the_floor();
    test_high_contrast_text_clears_7();
    test_high_contrast_controls_clear_4_5();
    test_high_contrast_ignores_the_scheme();
    test_high_contrast_off_is_unchanged();
    test_on_accent_is_white_without_high_contrast();
    test_high_contrast_follows_the_preference();
    /* Leave the palette as the rest of the suite expects to find it. */
    oc_theme_set_high_contrast(0);
    oc_theme_set_scheme(OC_SCHEME_MIDNIGHT);
    oc_theme_apply(OC_THEME_DARK);
    return failures;
}
