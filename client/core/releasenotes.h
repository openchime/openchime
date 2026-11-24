/*
 * OpenChime client — what is new in the release it is running.
 *
 * The notes are written once, by the control plane, from what the release
 * shipped. A client asks its own daemon for them by its build id
 * (oc_client_get_release_notes, PROTOCOL.md §5.16r), and the daemon asks
 * central with its workspace credential; the answer is central's,
 * `{ release, build_id, published_at, sections: [ { title, items: [ { title,
 * url } ] } ] }`, or 404 for a build that is not a release.
 * The same notes are a page at https://openchime.io/releases/build/<build id>.
 * A source build ("dev") is never a release and is not asked about.
 *
 * Read into fixed room, each count and length capped: a list longer than the
 * room is refused rather than cut short, since notes missing their end read as
 * the whole of them.
 */
#ifndef OC_RELEASENOTES_H
#define OC_RELEASENOTES_H

#include <stddef.h>

#define OC_RN_PAGE_URL  "https://openchime.io/releases/build/"
#define OC_RN_INDEX_URL "https://openchime.io/releases"

#define OC_RN_SECTIONS  12
#define OC_RN_ITEMS     60      /* in each section */
#define OC_RN_TEXT      240
#define OC_RN_URL       300
#define OC_RN_MAX_BODY  (256u * 1024u)

typedef struct { char title[OC_RN_TEXT]; char url[OC_RN_URL]; } oc_rn_item;
typedef struct { char title[OC_RN_TEXT]; int n_items; oc_rn_item items[OC_RN_ITEMS]; } oc_rn_section;
typedef struct {
    int  release;               /* its number */
    char date[32];              /* published_at, its date: "2026-11-11" */
    int  n_sections;
    oc_rn_section sections[OC_RN_SECTIONS];
} oc_releasenotes;

enum {
    OC_RN_OK = 0,
    OC_RN_NOT_RELEASE = 1,      /* a 404, or a source build */
    OC_RN_FAILED = 2            /* no answer, another status, or not the document */
};

/* Is `build_id` one a release could carry? "dev", empty and anything not a
 * plain token are not asked about. */
int oc_releasenotes_askable(const char *build_id);

/* The notes from a whole HTTP response (status line, headers, body). */
int oc_releasenotes_read_response(const char *resp, size_t len, oc_releasenotes *out);

/* The notes from a status and a body, as a browser's fetch hands them over. */
int oc_releasenotes_read(int status, const char *body, size_t len, oc_releasenotes *out);

#endif /* OC_RELEASENOTES_H */
