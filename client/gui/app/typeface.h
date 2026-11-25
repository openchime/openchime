/*
 * OpenChime GUI — the interface font: the short list a person chooses from,
 * how a choice becomes a family the system has, and the pinned line box and
 * baseline derived from that family's own figures (ARCH-97, ARCH-108).
 *
 * Pure: nothing here asks the system anything. Whether a family is installed
 * is a function the caller passes in (st_family_present in the client, a fake
 * in the tests), and the figures are numbers the caller read from the font
 * (st_family_metrics), so all of it runs in `make test`.
 */
#ifndef OC_GUI_TYPEFACE_H
#define OC_GUI_TYPEFACE_H

/* The fonts on offer, in the order the preference shows them. Default is the
 * platform's UI family; the rest are families Windows ships. A fixed list, not
 * any installed family: each one offered has been checked against the pinned
 * metrics and the clipping audit, which an arbitrary family cannot be. */
enum { OC_TYPEFACE_DEFAULT = 0, OC_TYPEFACE_VERDANA, OC_TYPEFACE_CALIBRI,
       OC_TYPEFACE_SITKA, OC_TYPEFACE_COUNT };

typedef struct {
    const char *label;    /* what the preference row says */
    const char *family;   /* the family asked for */
} oc_typeface;

extern const oc_typeface OC_TYPEFACES[OC_TYPEFACE_COUNT];

/* Whether the system has a family of that name. */
typedef int (*oc_typeface_present_fn)(void *user, const char *family);

/* The platform's UI family: Segoe UI Variable Text where the system has it,
 * else Segoe UI. */
const char *oc_typeface_default(oc_typeface_present_fn present, void *user);

/* The family to draw `family` with: itself when the system has it, else the
 * default, with *fell_back set — a missing family is never left to the
 * system's silent substitution. */
const char *oc_typeface_resolve_family(const char *family, oc_typeface_present_fn present,
                                       void *user, int *fell_back);

/* The family for a choice from the list; out of range is Default. */
const char *oc_typeface_resolve(int choice, oc_typeface_present_fn present, void *user,
                                int *fell_back);

/* Segoe UI's figures (unitsPerEm 2048: ascent 2210, cap height 1434), the
 * fallback when a family's own cannot be read. */
#define OC_TYPEFACE_SEGOE_ASC (2210.0f / 2048.0f)
#define OC_TYPEFACE_SEGOE_CAP (1434.0f / 2048.0f)

/* ARCH-108's pin for a format of `size` DIPs (text size applied) in a family
 * whose ascent and cap height are `ascent_em` and `cap_em` of its em:
 *   baseline = ceil(ascent)        — ink never rises past the line box's top;
 *   line_h   = round(2·bl − cap)   — the cap-ink centre on the box's centre.
 * A figure of 0 or less (not readable) takes Segoe UI's. */
void oc_typeface_pin(float size, float ascent_em, float cap_em,
                     float *line_h, float *baseline);

#endif /* OC_GUI_TYPEFACE_H */
