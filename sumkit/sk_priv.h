/* sumkit's own declarations, shared by its sources (sumkit.h is the API). */
#ifndef SK_PRIV_H
#define SK_PRIV_H

#include "nnkit.h"
#include "sumkit.h"

struct sk_file {
    const uint8_t *p;
    size_t         n;
    int            mapped;
    uint32_t       full;
    sk_config      cfg;
    sk_tensor     *t;
    size_t         count;
};

void     sk_seterr(char *err, size_t cap, const char *fmt, ...);


#endif
