/* The configuration registry's row (config.h): one per environment variable
 * the daemon reads. The table itself is in config_table.c; config.c reads it,
 * and main.c's --config-reference / --config-env / --check-config print it. */
#ifndef OC_CONFIG_TABLE_H
#define OC_CONFIG_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef enum {
    CFG_STR,      /* any string; stored as const char* (NULL when unset and no default) */
    CFG_PATH,     /* a path; CFG_MUST_EXIST makes it refuse when unreadable */
    CFG_INT,      /* an integer in [min, max]; stored as int */
    CFG_U64,      /* an unsigned integer in [min, max]; stored as uint64_t */
    CFG_BOOL,     /* 1 0 on off true false yes no; stored as int */
    CFG_ENUM,     /* one of `enums` ("a|b|c"); stored as int, the index */
    CFG_CUSTOM,   /* `parse` reads it (structured values) */
} cfg_kind;

typedef enum {
    CFG_OPERATOR, /* a setting or feature flag an operator decides: the reference's first part */
    CFG_MANAGED,  /* the control plane's, written at provision time; refused without its ticket */
    CFG_DEMO,     /* a demo build's test switch; refused by a production build */
} cfg_class;

enum {
    CFG_MUST_EXIST = 1,   /* PATH: must be readable when set */
    CFG_SECRET     = 2,   /* never printed: a key, a token */
    CFG_CLIENT     = 4,   /* read by the clients, not the daemon: known so the
                           * name is not "unknown" in a shared environment, and
                           * documented; never stored */
    CFG_PATTERN    = 8,   /* `name` carries %d; instances 1..`count` */
};

typedef struct cfg_row cfg_row;
struct cfg_row {
    const char *name;     /* OPENCHIME_… (a printf pattern with %d when CFG_PATTERN) */
    cfg_kind    kind;
    cfg_class   cls;
    const char *group;    /* the reference's heading */
    const char *dflt;     /* as a string, the way an operator would write it; NULL = unset */
    const char *doc;      /* one line */
    int64_t     min, max; /* INT / U64 */
    const char *enums;    /* ENUM: "a|b|c" */
    size_t      off;      /* offset into oc_config of the stored field */
    int       (*parse)(oc_config *c, const cfg_row *r, const char *name, const char *v,
                       char *err, size_t errcap);   /* CUSTOM */
    unsigned    flags;
    int         count;    /* PATTERN: instances 1..count */
};

/* The registry, terminated by a row with a NULL name. */
extern const cfg_row OC_CFG[];

/* Names that were read once and are not any more, each with what to do instead;
 * terminated by a NULL name. Set, they refuse the start with the message. */
typedef struct { const char *name, *instead; } cfg_removed;
extern const cfg_removed OC_CFG_REMOVED[];

#endif /* OC_CONFIG_TABLE_H */
