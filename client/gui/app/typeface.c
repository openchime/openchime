/*
 * OpenChime GUI — the interface font (ARCH-97, ARCH-108). See typeface.h.
 */

#include "typeface.h"

#include <math.h>
#include <stddef.h>

const oc_typeface OC_TYPEFACES[OC_TYPEFACE_COUNT] = {
    [OC_TYPEFACE_DEFAULT] = { "Default (Segoe UI)", "Segoe UI"   },
    [OC_TYPEFACE_VERDANA] = { "Verdana",            "Verdana"    },
    [OC_TYPEFACE_CALIBRI] = { "Calibri",            "Calibri"    },
    [OC_TYPEFACE_SITKA]   = { "Sitka",              "Sitka Text" },
};

const char *oc_typeface_default(oc_typeface_present_fn present, void *user) {
    /* The system's substitute for a missing "Segoe UI Variable Text" is not
     * necessarily Segoe UI, so it is asked for only where it is present. */
    return present && present(user, "Segoe UI Variable Text") ? "Segoe UI Variable Text" : "Segoe UI";
}

const char *oc_typeface_resolve_family(const char *family, oc_typeface_present_fn present,
                                       void *user, int *fell_back) {
    if (fell_back) *fell_back = 0;
    if (family && family[0] && present && present(user, family)) return family;
    if (fell_back) *fell_back = 1;
    return oc_typeface_default(present, user);
}

const char *oc_typeface_resolve(int choice, oc_typeface_present_fn present, void *user,
                                int *fell_back) {
    if (choice <= OC_TYPEFACE_DEFAULT || choice >= OC_TYPEFACE_COUNT) {
        /* Default is the platform's own family, whichever of the two it has. */
        if (fell_back) *fell_back = 0;
        return oc_typeface_default(present, user);
    }
    return oc_typeface_resolve_family(OC_TYPEFACES[choice].family, present, user, fell_back);
}

void oc_typeface_pin(float size, float ascent_em, float cap_em,
                     float *line_h, float *baseline) {
    if (!(ascent_em > 0.0f) || !(cap_em > 0.0f)) {
        ascent_em = OC_TYPEFACE_SEGOE_ASC;
        cap_em = OC_TYPEFACE_SEGOE_CAP;
    }
    /* The 0.02 keeps a size whose ascent lands a hair over a whole DIP (float
     * noise in size x text scale) from taking a whole extra row. */
    *baseline = ceilf(ascent_em * size - 0.02f);
    *line_h = roundf(2.0f * *baseline - cap_em * size);
}
