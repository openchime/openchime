/*
 * OpenChime Win32 GUI — the two palettes and the switch between them (WIN-26,
 * REQ-262). See theme.h.
 */

#include "theme.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
};

uint32_t oc_theme[TH_COUNT];
static int g_mode = -1;
static int g_scheme = OC_SCHEME_MIDNIGHT;

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

/* The user's app theme, from the same registry value the shell reads. Anything
 * unreadable means dark: this app was designed dark, so that is the safe miss. */
static int system_prefers_light(void) {
    HKEY k;
    DWORD v = 0, sz = sizeof v, type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    LONG rc = RegQueryValueExW(k, L"AppsUseLightTheme", NULL, &type, (LPBYTE)&v, &sz);
    RegCloseKey(k);
    return (rc == ERROR_SUCCESS && type == REG_DWORD && v == 1);
}

void oc_theme_apply(int mode) {
    if (mode < 0 || mode > OC_THEME_SYSTEM) mode = OC_THEME_SYSTEM;
    g_mode = mode;
    int light = (mode == OC_THEME_LIGHT) ||
                (mode == OC_THEME_SYSTEM && system_prefers_light());
    const uint32_t *src = light ? LIGHT : DARK;
    for (int i = 0; i < TH_COUNT; i++) oc_theme[i] = src[i];
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

const char *oc_theme_mode_name(int mode) {
    return mode == OC_THEME_LIGHT ? "Light"
         : mode == OC_THEME_SYSTEM ? "Match system" : "Dark";
}
