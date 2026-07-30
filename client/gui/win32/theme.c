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
static int g_accent = OC_ACCENT_BLUE;

/* name, dark accent, dark dim, light accent, light dim. The BLUE row is exactly
 * what the two palettes above already carry, so the default is not a new colour
 * scheme arriving with the feature. */
static const struct { const char *name; uint32_t d, dd, l, ld; } ACCENTS[OC_ACCENT_COUNT] = {
    { "Blue",   0x3D8BFF, 0x2563EB, 0x1264A3, 0x0B4C7F },
    { "Indigo", 0x8B7CFF, 0x6D5AE0, 0x4F46B5, 0x3B3490 },
    { "Teal",   0x2DD4BF, 0x14A99B, 0x0F766E, 0x0A5750 },
    { "Plum",   0xE879C7, 0xC2569F, 0xA1367F, 0x7C2762 },
};

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
    if (mode < 0 || mode > OC_THEME_SYSTEM) mode = OC_THEME_DARK;
    g_mode = mode;
    int light = (mode == OC_THEME_LIGHT) ||
                (mode == OC_THEME_SYSTEM && system_prefers_light());
    const uint32_t *src = light ? LIGHT : DARK;
    for (int i = 0; i < TH_COUNT; i++) oc_theme[i] = src[i];
    /* The accent is applied AFTER the palette copy, not folded into the tables:
     * otherwise every accent would need its own full palette and the two would
     * drift the first time a neutral changed. */
    oc_theme[TH_ACCENT]     = light ? ACCENTS[g_accent].l  : ACCENTS[g_accent].d;
    oc_theme[TH_ACCENT_DIM] = light ? ACCENTS[g_accent].ld : ACCENTS[g_accent].dd;
}

void oc_theme_set_accent(int accent) {
    if (accent < 0 || accent >= OC_ACCENT_COUNT) accent = OC_ACCENT_BLUE;
    g_accent = accent;
    oc_theme_apply(oc_theme_mode());      /* re-resolve; the mode decides which pair */
}

int oc_theme_accent(void) { return g_accent; }

const char *oc_theme_accent_name(int accent) {
    if (accent < 0 || accent >= OC_ACCENT_COUNT) accent = OC_ACCENT_BLUE;
    return ACCENTS[accent].name;
}

uint32_t oc_theme_accent_swatch(int accent) {
    if (accent < 0 || accent >= OC_ACCENT_COUNT) accent = OC_ACCENT_BLUE;
    int light = (g_mode == OC_THEME_LIGHT) ||
                (g_mode == OC_THEME_SYSTEM && system_prefers_light());
    return light ? ACCENTS[accent].l : ACCENTS[accent].d;
}

int oc_theme_mode(void) { return g_mode < 0 ? OC_THEME_DARK : g_mode; }

const char *oc_theme_mode_name(int mode) {
    return mode == OC_THEME_LIGHT ? "Light"
         : mode == OC_THEME_SYSTEM ? "Match system" : "Dark";
}
