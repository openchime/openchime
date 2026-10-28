/* What makes a message an action (REQ-058, ARCH-115). See action.h. */
#include "action.h"

#include <string.h>

int oc_action_parse(const char *body, size_t len, uint32_t *start, uint32_t *tlen) {
    if (!body || len < 5 || len > UINT32_MAX || memcmp(body, "/me", 3) != 0 || body[3] != ' ') return 0;
    size_t i = 4;
    while (i < len && body[i] == ' ') i++;
    if (i == len) return 0;
    char c = body[i];
    if (c == '\t' || c == '\r' || c == '\n') return 0;
    *start = (uint32_t)i;
    *tlen  = (uint32_t)(len - i);
    return 1;
}
