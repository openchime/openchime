/*
 * OpenChime GUI — the two palettes and the switch between them (REQ-262). See theme.h.
 */

#include "theme.h"
#include "protocol.h"   /* OC_PRESENCE_* */

#include <math.h>

/* Dark: a deep-blue accent on a dark neutral shell, with real elevation steps
 * (darkest rail -> lightest canvas) so the panes read as distinct layers. */
static const uint32_t DARK[TH_COUNT] = {
    [TH_ACCENT]     = 0x3D8BFF,
    [TH_ACCENT_DIM] = 0x2563EB,
    [TH_RAIL]       = 0x0B1220,
    [TH_SIDEBAR]    = 0x111C33,
    [TH_BASE]       = 0x162238,
    [TH_HEADER]     = 0x111C33,
    [TH_INPUT]      = 0x1E2E4C,
    [TH_SELECT]     = 0x2C4E86,
    [TH_HOVER]      = 0x1D2C48,
    [TH_BORDER]     = 0x27395C,
    [TH_RAIL_ICON]  = 0xC4C9D3,
    [TH_TEXT]       = 0xE9EDF5,
    [TH_MUTED]      = 0x93A1BC,
    [TH_FAINT]      = 0x63708C,
    [TH_DANGER]     = 0xE05252,
    [TH_NOTICE]     = 0x14B8A6,
    [TH_ONLINE]     = 0x3BA55D,
    [TH_AWAY]       = 0xD9A441,
    [TH_ON_ACCENT]  = 0xFFFFFF,
};

/* Light: the same *structure* inverted, not merely brighter colours — the rail
 * stays the darkest surface and the canvas the lightest, so the elevation the
 * layout depends on survives the switch. The accent darkens because the bright
 * blue that pops on navy is unreadable on white. */
static const uint32_t LIGHT[TH_COUNT] = {
    [TH_ACCENT]     = 0x1264A3,
    [TH_ACCENT_DIM] = 0x0B4C7F,
    [TH_RAIL]       = 0x3F0E40,   /* the aubergine rail is Slack's, and it works */
    [TH_SIDEBAR]    = 0xF3F4F6,
    [TH_BASE]       = 0xFFFFFF,
    [TH_HEADER]     = 0xF3F4F6,
    [TH_INPUT]      = 0xFFFFFF,
    [TH_SELECT]     = 0xCFE3F7,
    [TH_HOVER]      = 0xE8EAED,
    [TH_BORDER]     = 0xD5D8DE,
    [TH_RAIL_ICON]  = 0xD9CCDA,
    [TH_TEXT]       = 0x1D1C1D,
    [TH_MUTED]      = 0x5B5F66,
    [TH_FAINT]      = 0x868A91,
    [TH_DANGER]     = 0xB3261E,
    [TH_NOTICE]     = 0x0F766E,
    [TH_ONLINE]     = 0x2A8544,
    [TH_AWAY]       = 0x9A6A00,
    [TH_ON_ACCENT]  = 0xFFFFFF,
};

/* HIGH CONTRAST (REQ-262): one fixed palette per mode, used in place of the two
 * above and of every colour scheme while it is on. The structure is the same --
 * the rail darkest, the canvas lightest in light, the panes as distinct layers --
 * but every text ink (TEXT, MUTED, FAINT, ACCENT, DANGER) clears 7:1 on every
 * surface it is painted on, and every control ink (BORDER, ACCENT_DIM, ONLINE,
 * AWAY, NOTICE) 4.5:1. tests/test_theme.c asserts both from these tables.
 *
 * Dark: a black canvas and near-black panes, near-white inks, and accents light
 * enough to read on black -- which is why the ink ON an accent fill is black
 * here: no accent can clear 7:1 against black and carry white text at 7:1 too. */
static const uint32_t HC_DARK[TH_COUNT] = {
    [TH_ACCENT]     = 0x80C4FF,
    [TH_ACCENT_DIM] = 0x4FA3F7,
    [TH_RAIL]       = 0x000000,
    [TH_SIDEBAR]    = 0x0A0A0A,
    [TH_BASE]       = 0x000000,
    [TH_HEADER]     = 0x0A0A0A,
    [TH_INPUT]      = 0x141414,
    [TH_SELECT]     = 0x142640,
    [TH_HOVER]      = 0x1E1E1E,
    [TH_BORDER]     = 0x9E9E9E,
    [TH_RAIL_ICON]  = 0xFFFFFF,
    [TH_TEXT]       = 0xFFFFFF,
    [TH_MUTED]      = 0xDCDCDC,
    [TH_FAINT]      = 0xC8C8C8,
    [TH_DANGER]     = 0xFFA098,
    [TH_NOTICE]     = 0x3EE0CB,
    [TH_ONLINE]     = 0x4ADE80,
    [TH_AWAY]       = 0xFFC94D,
    [TH_ON_ACCENT]  = 0x000000,
};

/* Light: a white canvas, black inks, and accents dark enough for white on
 * them; the rail is black, so its icons stay white. */
static const uint32_t HC_LIGHT[TH_COUNT] = {
    [TH_ACCENT]     = 0x0040A0,
    [TH_ACCENT_DIM] = 0x00307A,
    [TH_RAIL]       = 0x000000,
    [TH_SIDEBAR]    = 0xF5F5F5,
    [TH_BASE]       = 0xFFFFFF,
    [TH_HEADER]     = 0xF5F5F5,
    [TH_INPUT]      = 0xFFFFFF,
    [TH_SELECT]     = 0xD6E6FA,
    [TH_HOVER]      = 0xEBEBEB,
    [TH_BORDER]     = 0x5E5E5E,
    [TH_RAIL_ICON]  = 0xFFFFFF,
    [TH_TEXT]       = 0x000000,
    [TH_MUTED]      = 0x262626,
    [TH_FAINT]      = 0x383838,
    [TH_DANGER]     = 0x8B0000,
    [TH_NOTICE]     = 0x005C55,
    [TH_ONLINE]     = 0x0A6630,
    [TH_AWAY]       = 0x6B4500,
    [TH_ON_ACCENT]  = 0xFFFFFF,
};

uint32_t oc_theme[TH_COUNT];
static int g_mode = -1;
static int g_scheme = OC_SCHEME_MIDNIGHT;
static int g_high_contrast = 0;

/* The channel colours, one set per palette (oc_theme_chan_color): blue,
 * indigo, violet, pink, teal, cyan, slate, brown. Lighter on the dark
 * palettes, deeper on the light ones, so each clears 3:1 on the sidebar. */
static const uint32_t CHAN_DARK[8]     = { 0x60A5FA, 0x818CF8, 0xA78BFA, 0xF472B6,
                                           0x2DD4BF, 0x22D3EE, 0x94A3B8, 0xC8946A };
static const uint32_t CHAN_LIGHT[8]    = { 0x1D4ED8, 0x4338CA, 0x6D28D9, 0xBE185D,
                                           0x0F766E, 0x0E7490, 0x475569, 0x8A4B22 };
static const uint32_t CHAN_HC_DARK[8]  = { 0x7AB8FF, 0x9FA8FF, 0xC0A8FF, 0xFF8CCB,
                                           0x4EEAD6, 0x5BE3F5, 0xB8C4D4, 0xE0AE84 };
static const uint32_t CHAN_HC_LIGHT[8] = { 0x1E40AF, 0x3730A3, 0x5B21B6, 0x9D174D,
                                           0x115E59, 0x155E75, 0x334155, 0x6F3B19 };
static const uint32_t *g_chan = CHAN_DARK;

/* A scheme is the RAIL and the ACCENT together, per mode:
 *   name, dark rail, light rail, dark accent, dark dim, light accent, light dim.
 *
 * MIDNIGHT is exactly what the two palettes above already carry — the navy rail in
 * dark, Slack's aubergine in light — so the default is not a new colour scheme
 * arriving with the feature.
 *
 * Every rail is dark in BOTH modes on purpose: the rail's icons and labels are
 * near-white (TH_RAIL_ICON), so a light rail would need its own foreground set and
 * a second contrast problem to keep solved. Slack's light theme keeps a dark rail
 * for the same reason. */
static const struct {
    const char *name;
    uint32_t rail_d, rail_l;
    uint32_t d, dd, l, ld;
} SCHEMES[OC_SCHEME_COUNT] = {
    { "Midnight", 0x101A2E, 0x21324F, 0x3D8BFF, 0x2563EB, 0x1264A3, 0x0B4C7F },
    { "Indigo",   0x241B4D, 0x3A2E7A, 0x8B7CFF, 0x6D5AE0, 0x4F46B5, 0x3B3490 },
    { "Teal",     0x0B322D, 0x134A43, 0x2DD4BF, 0x14A99B, 0x0F766E, 0x0A5750 },
    { "Plum",     0x3B1039, 0x4A1240, 0xE879C7, 0xC2569F, 0xA1367F, 0x7C2762 },
};

/* Mix two 0xRRGGBB colours, `t` in 0..1 of `a` over `b`. Per channel, because the
 * point is a colour that belongs to BOTH — a selected row has to read as the accent
 * without becoming a second accent. */
static uint32_t mix(uint32_t a, uint32_t b, float t) {
    float u = 1.0f - t;
    unsigned r = (unsigned)(((a >> 16) & 0xFF) * t + ((b >> 16) & 0xFF) * u);
    unsigned g = (unsigned)(((a >>  8) & 0xFF) * t + ((b >>  8) & 0xFF) * u);
    unsigned bl = (unsigned)((a & 0xFF) * t + (b & 0xFF) * u);
    return (r << 16) | (g << 8) | bl;
}

/* The desktop's light-or-dark preference, asked through the function the
 * application hands in (oc_plat_prefers_light) rather than called by name:
 * this file is compiled into the test binary, which has no platform layer, and
 * the tests drive LIGHT and DARK explicitly rather than through SYSTEM. Asked
 * on every resolution, so a change made in the desktop's settings shows the
 * next time the theme is resolved. Anything but 1 means dark: this app was
 * designed dark, so that is the safe miss. */
static int (*g_prefers_light)(void);

void oc_theme_set_system_query(int (*prefers_light)(void)) { g_prefers_light = prefers_light; }

static int system_prefers_light(void) { return g_prefers_light && g_prefers_light() == 1; }

/* --- the contrast guarantee (REQ-262) --------------------------------------
 *
 * An ink token is legible on some surfaces and not others, and until this
 * existed nothing said which. FAINT reads well enough on the canvas and the
 * sidebar and falls apart on everything a row or a field is filled with: the
 * composer cue sat at 2.73:1 on the input, the draft pencil at 2.03:1 on a
 * selected row under the default scheme and 1.58:1 under Teal. WCAG asks 3:1 of
 * non-text UI and more of text; below 3:1 is not dim, it is gone.
 *
 * The rule is a PAIR, so it is asked as a pair. oc_ink_on(ink, surface) returns
 * the ink if it clears the floor there and the next-stronger one if it does not,
 * which keeps the two-step hierarchy exactly where the hierarchy is legible and
 * abandons it only where it was never readable. That is why this is not a
 * retuned FAINT: to clear the worst surface a single colour would have to move
 * so far it became MUTED, and the palette would keep two names for one colour.
 *
 * Resolved once per theme apply rather than per paint. TH_SELECT is derived from
 * the scheme, so there are eight of it and none of them are constants — a table
 * computed after the derivation covers every scheme without naming any. */

static float srgb_ch(unsigned v) {
    float c = (float)v / 255.0f;
    return c <= 0.03928f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

float oc_theme_luminance(uint32_t rgb) {
    return 0.2126f * srgb_ch((rgb >> 16) & 0xFF) +
           0.7152f * srgb_ch((rgb >>  8) & 0xFF) +
           0.0722f * srgb_ch(rgb & 0xFF);
}

float oc_theme_contrast(uint32_t a, uint32_t b) {
    float la = oc_theme_luminance(a), lb = oc_theme_luminance(b);
    if (la < lb) { float t = la; la = lb; lb = t; }
    return (la + 0.05f) / (lb + 0.05f);
}

/* The escalation order, weakest first. Asking for a rung returns that rung or a
 * later one, never an earlier: an ink may be strengthened to stay legible, never
 * weakened to stay pretty. */
static const int INK_LADDER[] = { TH_FAINT, TH_MUTED, TH_TEXT };
#define INK_LADDER_N ((int)(sizeof INK_LADDER / sizeof INK_LADDER[0]))

/* [ink rung][surface token] -> the token to actually use. */
static uint8_t g_ink_on[INK_LADDER_N][TH_COUNT];

static void resolve_ink_table(void) {
    for (int r = 0; r < INK_LADDER_N; r++) {
        for (int s = 0; s < TH_COUNT; s++) {
            int chosen = INK_LADDER[r];
            for (int k = r; k < INK_LADDER_N; k++) {
                chosen = INK_LADDER[k];
                if (oc_theme_contrast(oc_theme[chosen], oc_theme[s]) >= OC_CONTRAST_FLOOR)
                    break;
            }
            /* Falling out of that loop without clearing leaves the strongest
             * rung, which is the most legible thing the palette has. A surface
             * that TEXT cannot clear is a broken surface, and the test says so
             * rather than this silently papering over it. */
            g_ink_on[r][s] = (uint8_t)chosen;
        }
    }
}

uint32_t oc_ink_on(int ink, int surface) {
    if (surface < 0 || surface >= TH_COUNT) return oc_theme[ink];
    for (int r = 0; r < INK_LADDER_N; r++)
        if (INK_LADDER[r] == ink) return oc_theme[g_ink_on[r][surface]];
    return oc_theme[ink];      /* not a laddered ink: DANGER, ONLINE and friends */
}

void oc_theme_apply(int mode) {
    if (mode < 0 || mode > OC_THEME_SYSTEM) mode = OC_THEME_SYSTEM;
    g_mode = mode;
    int light = (mode == OC_THEME_LIGHT) ||
                (mode == OC_THEME_SYSTEM && system_prefers_light());
    if (g_high_contrast) {
        /* A fixed palette: no scheme and no derived selection, so nothing a
         * scheme picks can pull a pair under its ratio. The ink table is still
         * resolved, so OC_INK_ON answers as it always does. */
        const uint32_t *hc = light ? HC_LIGHT : HC_DARK;
        for (int i = 0; i < TH_COUNT; i++) oc_theme[i] = hc[i];
        g_chan = light ? CHAN_HC_LIGHT : CHAN_HC_DARK;
        resolve_ink_table();
        return;
    }
    const uint32_t *src = light ? LIGHT : DARK;
    for (int i = 0; i < TH_COUNT; i++) oc_theme[i] = src[i];
    g_chan = light ? CHAN_LIGHT : CHAN_DARK;
    /* The scheme is applied AFTER the palette copy, not folded into the tables:
     * otherwise every scheme would need its own full palette and the two would drift
     * the first time a neutral changed. */
    oc_theme[TH_ACCENT]     = light ? SCHEMES[g_scheme].l  : SCHEMES[g_scheme].d;
    oc_theme[TH_ACCENT_DIM] = light ? SCHEMES[g_scheme].ld : SCHEMES[g_scheme].dd;
    oc_theme[TH_RAIL]       = light ? SCHEMES[g_scheme].rail_l : SCHEMES[g_scheme].rail_d;
    /* The SELECTED row follows the scheme too. It is the accent at low strength over
     * the sidebar, not a colour of its own: a fixed blue selection under a plum
     * scheme looked like a bug rather than a neutral, and it is the second most
     * prominent coloured surface after the rail. */
    oc_theme[TH_SELECT] = mix(oc_theme[TH_ACCENT], oc_theme[TH_SIDEBAR], light ? 0.22f : 0.34f);
    /* Last, because it reads every colour above it — including the SELECT that
     * was just derived. */
    resolve_ink_table();
}

void oc_theme_set_scheme(int scheme) {
    if (scheme < 0 || scheme >= OC_SCHEME_COUNT) scheme = OC_SCHEME_MIDNIGHT;
    g_scheme = scheme;
    oc_theme_apply(oc_theme_mode());      /* re-resolve; the mode decides which pair */
}

int oc_theme_scheme(void) { return g_scheme; }

const char *oc_theme_scheme_name(int scheme) {
    if (scheme < 0 || scheme >= OC_SCHEME_COUNT) scheme = OC_SCHEME_MIDNIGHT;
    return SCHEMES[scheme].name;
}

static int scheme_light(void) {
    return (g_mode == OC_THEME_LIGHT) ||
           (g_mode == OC_THEME_SYSTEM && system_prefers_light());
}

int oc_theme_is_light(void) { return scheme_light(); }

uint32_t oc_theme_scheme_accent(int scheme) {
    if (scheme < 0 || scheme >= OC_SCHEME_COUNT) scheme = OC_SCHEME_MIDNIGHT;
    return scheme_light() ? SCHEMES[scheme].l : SCHEMES[scheme].d;
}

uint32_t oc_theme_scheme_rail(int scheme) {
    if (scheme < 0 || scheme >= OC_SCHEME_COUNT) scheme = OC_SCHEME_MIDNIGHT;
    return scheme_light() ? SCHEMES[scheme].rail_l : SCHEMES[scheme].rail_d;
}

int oc_theme_mode(void) { return g_mode < 0 ? OC_THEME_SYSTEM : g_mode; }

int oc_high_contrast_on(int pref, int sys) {
    if (pref == OC_HC_ON)  return 1;
    if (pref == OC_HC_OFF) return 0;
    return sys == 1;      /* Follow system, and anything outside the three */
}

void oc_theme_set_high_contrast(int on) {
    g_high_contrast = on ? 1 : 0;
    oc_theme_apply(oc_theme_mode());      /* re-resolve; the mode decides which palette */
}

int oc_theme_high_contrast(void) { return g_high_contrast; }

const char *oc_theme_mode_name(int mode) {
    return mode == OC_THEME_LIGHT ? "Light"
         : mode == OC_THEME_SYSTEM ? "Match system" : "Dark";
}

/* The marker's shape (REQ-122): see theme.h. Do-not-disturb wins, because it is
 * the one a colleague most needs to see before writing. */
int oc_presence_mark(uint8_t presence, int dnd) {
    if (dnd) return OC_PMARK_DND;
    if (presence == OC_PRESENCE_ONLINE) return OC_PMARK_ONLINE;
    if (presence == OC_PRESENCE_AWAY)   return OC_PMARK_AWAY;
    return OC_PMARK_OFFLINE;
}

uint32_t oc_theme_chan_color(int idx) {
    if (idx < 1 || idx > 8) return 0;
    return g_chan[idx - 1];
}
