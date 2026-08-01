/*
 * OpenChime client core — composer completion and the emoji catalogue.
 * See complete.h.
 */

#include "complete.h"
#include "protocol.h"      /* OC_CHANNEL_KIND_DM */

#include <ctype.h>
#include <string.h>
#include <stdio.h>

/* ---- the catalogue --------------------------------------------------------
 * Ordered by category so a picker can walk it once and emit section headers
 * without sorting. Shortcodes follow the names people already type elsewhere. */
#define S OC_EMOJI_CAT_SMILEYS
#define G OC_EMOJI_CAT_GESTURES
#define P OC_EMOJI_CAT_PEOPLE
#define N OC_EMOJI_CAT_NATURE
#define F OC_EMOJI_CAT_FOOD
#define A OC_EMOJI_CAT_ACTIVITY
#define O OC_EMOJI_CAT_OBJECTS
#define Y OC_EMOJI_CAT_SYMBOLS

static const oc_emoji EMOJI[] = {
    /* smileys */
    {"smile","\xf0\x9f\x98\x84","happy joy",S},
    {"grin","\xf0\x9f\x98\x81","happy",S},
    {"laughing","\xf0\x9f\x98\x86","lol haha",S},
    {"joy","\xf0\x9f\x98\x82","lol crying laugh",S},
    {"rofl","\xf0\x9f\xa4\xa3","lol rolling",S},
    {"slightly_smiling","\xf0\x9f\x99\x82","",S},
    {"wink","\xf0\x9f\x98\x89","",S},
    {"blush","\xf0\x9f\x98\x8a","shy",S},
    {"heart_eyes","\xf0\x9f\x98\x8d","love",S},
    {"kissing_heart","\xf0\x9f\x98\x98","kiss",S},
    {"thinking","\xf0\x9f\xa4\x94","hmm",S},
    {"neutral","\xf0\x9f\x98\x90","",S},
    {"expressionless","\xf0\x9f\x98\x91","",S},
    {"unamused","\xf0\x9f\x98\x92","meh",S},
    {"roll_eyes","\xf0\x9f\x99\x84","",S},
    {"smirk","\xf0\x9f\x98\x8f","",S},
    {"grimacing","\xf0\x9f\x98\xac","",S},
    {"zipper_mouth","\xf0\x9f\xa4\x90","quiet",S},
    {"sunglasses","\xf0\x9f\x98\x8e","cool",S},
    {"nerd","\xf0\x9f\xa4\x93","",S},
    {"sweat_smile","\xf0\x9f\x98\x85","phew",S},
    {"sweat","\xf0\x9f\x98\x93","",S},
    {"cry","\xf0\x9f\x98\xa2","sad",S},
    {"sob","\xf0\x9f\x98\xad","crying sad",S},
    {"scream","\xf0\x9f\x98\xb1","shock",S},
    {"fearful","\xf0\x9f\x98\xa8","",S},
    {"weary","\xf0\x9f\x98\xa9","tired",S},
    {"tired_face","\xf0\x9f\x98\xab","",S},
    {"triumph","\xf0\x9f\x98\xa4","",S},
    {"rage","\xf0\x9f\x98\xa1","angry mad",S},
    {"angry","\xf0\x9f\x98\xa0","mad",S},
    {"exploding_head","\xf0\x9f\xa4\xaf","mindblown",S},
    {"hot","\xf0\x9f\xa5\xb5","",S},
    {"cold","\xf0\x9f\xa5\xb6","",S},
    {"woozy","\xf0\x9f\xa5\xb4","",S},
    {"upside_down","\xf0\x9f\x99\x83","",S},
    {"melting","\xf0\x9f\xab\xa0","",S},
    {"shushing","\xf0\x9f\xa4\xab","quiet",S},
    {"salute","\xf0\x9f\xab\xa1","",S},
    {"innocent","\xf0\x9f\x98\x87","angel",S},
    {"party_face","\xf0\x9f\xa5\xb3","celebrate",S},
    {"sleeping","\xf0\x9f\x98\xb4","zzz",S},
    {"mask","\xf0\x9f\x98\xb7","sick",S},
    {"poop","\xf0\x9f\x92\xa9","",S},
    {"ghost","\xf0\x9f\x91\xbb","",S},
    {"skull","\xf0\x9f\x92\x80","dead",S},
    {"alien","\xf0\x9f\x91\xbd","",S},
    {"robot","\xf0\x9f\xa4\x96","bot",S},

    /* gestures */
    {"+1","\xf0\x9f\x91\x8d","thumbsup yes approve",G},
    {"thumbsup","\xf0\x9f\x91\x8d","+1 yes",G},
    {"-1","\xf0\x9f\x91\x8e","thumbsdown no",G},
    {"thumbsdown","\xf0\x9f\x91\x8e","-1 no",G},
    {"ok_hand","\xf0\x9f\x91\x8c","ok",G},
    {"ok","\xf0\x9f\x91\x8c","",G},
    {"clap","\xf0\x9f\x91\x8f","applause",G},
    {"raised_hands","\xf0\x9f\x99\x8c","celebrate",G},
    {"pray","\xf0\x9f\x99\x8f","thanks please",G},
    {"wave","\xf0\x9f\x91\x8b","hello bye",G},
    {"muscle","\xf0\x9f\x92\xaa","strong",G},
    {"point_up","\xe2\x98\x9d\xef\xb8\x8f","",G},
    {"point_right","\xf0\x9f\x91\x89","",G},
    {"point_left","\xf0\x9f\x91\x88","",G},
    {"point_down","\xf0\x9f\x91\x87","",G},
    {"raised_hand","\xe2\x9c\x8b","stop",G},
    {"fist","\xe2\x9c\x8a","",G},
    {"handshake","\xf0\x9f\xa4\x9d","deal",G},
    {"writing_hand","\xe2\x9c\x8d\xef\xb8\x8f","",G},
    {"crossed_fingers","\xf0\x9f\xa4\x9e","luck",G},

    /* people */
    {"eyes","\xf0\x9f\x91\x80","looking watching",P},
    {"brain","\xf0\x9f\xa7\xa0","",P},
    {"person","\xf0\x9f\xa7\x91","",P},
    {"technologist","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x92\xbb","developer coding",P},
    {"detective","\xf0\x9f\x95\xb5\xef\xb8\x8f","investigate",P},
    {"construction_worker","\xf0\x9f\x91\xb7","wip",P},
    {"firefighter","\xf0\x9f\xa7\x91\xe2\x80\x8d\xf0\x9f\x9a\x92","incident",P},

    /* nature */
    {"fire","\xf0\x9f\x94\xa5","lit hot",N},
    {"star","\xe2\xad\x90","",N},
    {"sparkles","\xe2\x9c\xa8","shiny new",N},
    {"zap","\xe2\x9a\xa1","lightning fast",N},
    {"boom","\xf0\x9f\x92\xa5","explosion",N},
    {"rainbow","\xf0\x9f\x8c\x88","",N},
    {"sun","\xe2\x98\x80\xef\xb8\x8f","sunny",N},
    {"cloud","\xe2\x98\x81\xef\xb8\x8f","",N},
    {"snowflake","\xe2\x9d\x84\xef\xb8\x8f","cold freeze",N},
    {"ocean","\xf0\x9f\x8c\x8a","wave water",N},
    {"seedling","\xf0\x9f\x8c\xb1","growth",N},
    {"leaves","\xf0\x9f\x8d\x83","",N},
    {"bug","\xf0\x9f\x90\x9b","defect",N},
    {"snail","\xf0\x9f\x90\x8c","slow",N},
    {"turtle","\xf0\x9f\x90\xa2","slow",N},
    {"rocket","\xf0\x9f\x9a\x80","ship launch deploy",N},
    {"dog","\xf0\x9f\x90\xb6","",N},
    {"cat","\xf0\x9f\x90\xb1","",N},
    {"unicorn","\xf0\x9f\xa6\x84","",N},
    {"whale","\xf0\x9f\x90\xb3","docker",N},
    {"penguin","\xf0\x9f\x90\xa7","linux",N},

    /* food */
    {"coffee","\xe2\x98\x95","caffeine",F},
    {"tea","\xf0\x9f\x8d\xb5","",F},
    {"beer","\xf0\x9f\x8d\xba","",F},
    {"wine","\xf0\x9f\x8d\xb7","",F},
    {"champagne","\xf0\x9f\x8d\xbe","celebrate",F},
    {"pizza","\xf0\x9f\x8d\x95","",F},
    {"hamburger","\xf0\x9f\x8d\x94","burger",F},
    {"taco","\xf0\x9f\x8c\xae","",F},
    {"doughnut","\xf0\x9f\x8d\xa9","donut",F},
    {"cookie","\xf0\x9f\x8d\xaa","",F},
    {"cake","\xf0\x9f\x8e\x82","birthday",F},
    {"popcorn","\xf0\x9f\x8d\xbf","watching",F},
    {"apple","\xf0\x9f\x8d\x8e","",F},
    {"avocado","\xf0\x9f\xa5\x91","",F},
    {"salt","\xf0\x9f\xa7\x82","salty",F},

    /* activity */
    {"tada","\xf0\x9f\x8e\x89","party celebrate ship",A},
    {"confetti","\xf0\x9f\x8e\x8a","party",A},
    {"trophy","\xf0\x9f\x8f\x86","win",A},
    {"medal","\xf0\x9f\x8f\x85","",A},
    {"dart","\xf0\x9f\x8e\xaf","target bullseye",A},
    {"game_die","\xf0\x9f\x8e\xb2","random",A},
    {"soccer","\xe2\x9a\xbd","football",A},
    {"basketball","\xf0\x9f\x8f\x80","",A},
    {"guitar","\xf0\x9f\x8e\xb8","",A},
    {"art","\xf0\x9f\x8e\xa8","design",A},
    {"clapper","\xf0\x9f\x8e\xac","action",A},

    /* objects */
    {"bulb","\xf0\x9f\x92\xa1","idea",O},
    {"wrench","\xf0\x9f\x94\xa7","fix tool",O},
    {"hammer","\xf0\x9f\x94\xa8","build",O},
    {"gear","\xe2\x9a\x99\xef\xb8\x8f","settings config",O},
    {"lock","\xf0\x9f\x94\x92","private secure",O},
    {"unlock","\xf0\x9f\x94\x93","",O},
    {"key","\xf0\x9f\x94\x91","",O},
    {"mag","\xf0\x9f\x94\x8d","search find",O},
    {"link","\xf0\x9f\x94\x97","url",O},
    {"paperclip","\xf0\x9f\x93\x8e","attachment",O},
    {"pushpin","\xf0\x9f\x93\x8c","pin",O},
    {"calendar","\xf0\x9f\x93\x85","date",O},
    {"chart","\xf0\x9f\x93\x88","graph metrics up",O},
    {"chart_down","\xf0\x9f\x93\x89","graph metrics",O},
    {"memo","\xf0\x9f\x93\x9d","note write",O},
    {"books","\xf0\x9f\x93\x9a","docs",O},
    {"package","\xf0\x9f\x93\xa6","release",O},
    {"inbox","\xf0\x9f\x93\xa5","",O},
    {"outbox","\xf0\x9f\x93\xa4","",O},
    {"bell","\xf0\x9f\x94\x94","notification",O},
    {"no_bell","\xf0\x9f\x94\x95","mute dnd",O},
    {"phone","\xf0\x9f\x93\x9e","call",O},
    {"computer","\xf0\x9f\x92\xbb","laptop",O},
    {"floppy","\xf0\x9f\x92\xbe","save",O},
    {"battery","\xf0\x9f\x94\x8b","",O},
    {"hourglass","\xe2\x8f\xb3","waiting",O},
    {"alarm","\xe2\x8f\xb0","reminder",O},
    {"money","\xf0\x9f\x92\xb0","cost budget",O},
    {"gift","\xf0\x9f\x8e\x81","present",O},
    {"broom","\xf0\x9f\xa7\xb9","cleanup",O},
    {"magnet","\xf0\x9f\xa7\xb2","",O},
    {"test_tube","\xf0\x9f\xa7\xaa","experiment test",O},
    {"microscope","\xf0\x9f\x94\xac","research",O},
    {"telescope","\xf0\x9f\x94\xad","",O},
    {"shield","\xf0\x9f\x9b\xa1\xef\xb8\x8f","security",O},

    /* symbols */
    {"heart","\xe2\x9d\xa4\xef\xb8\x8f","love",Y},
    {"broken_heart","\xf0\x9f\x92\x94","",Y},
    {"100","\xf0\x9f\x92\xaf","perfect",Y},
    {"check","\xe2\x9c\x85","done yes tick",Y},
    {"heavy_check","\xe2\x9c\x94\xef\xb8\x8f","done",Y},
    {"x","\xe2\x9d\x8c","no fail",Y},
    {"warning","\xe2\x9a\xa0\xef\xb8\x8f","caution",Y},
    {"question","\xe2\x9d\x93","",Y},
    {"exclamation","\xe2\x9d\x97","",Y},
    {"no_entry","\xe2\x9b\x94","blocked stop",Y},
    {"recycle","\xe2\x99\xbb\xef\xb8\x8f","retry",Y},
    {"arrow_up","\xe2\xac\x86\xef\xb8\x8f","",Y},
    {"arrow_down","\xe2\xac\x87\xef\xb8\x8f","",Y},
    {"arrows_counterclockwise","\xf0\x9f\x94\x84","refresh sync",Y},
    {"white_check_mark","\xe2\x9c\x85","done",Y},
    {"red_circle","\xf0\x9f\x94\xb4","",Y},
    {"green_circle","\xf0\x9f\x9f\xa2","",Y},
    {"yellow_circle","\xf0\x9f\x9f\xa1","",Y},
    {"eyes_symbol","\xf0\x9f\x91\x81\xef\xb8\x8f","",Y},
    {"speech_balloon","\xf0\x9f\x92\xac","comment",Y},
    {"thought_balloon","\xf0\x9f\x92\xad","",Y},
    {"zzz","\xf0\x9f\x92\xa4","sleep idle",Y},
};

#undef S
#undef G
#undef P
#undef N
#undef F
#undef A
#undef O
#undef Y

static const char *CAT_NAMES[OC_EMOJI_CAT_COUNT] = {
    "Smileys", "Gestures", "People", "Nature", "Food", "Activity", "Objects", "Symbols"
};

const oc_emoji *oc_emoji_all(size_t *count) {
    if (count) *count = sizeof EMOJI / sizeof *EMOJI;
    return EMOJI;
}

const char *oc_emoji_category_name(uint8_t category) {
    return category < OC_EMOJI_CAT_COUNT ? CAT_NAMES[category] : "";
}

const char *oc_emoji_by_name(const char *name) {
    if (!name || !name[0]) return NULL;
    for (size_t i = 0; i < sizeof EMOJI / sizeof *EMOJI; i++)
        if (strcmp(EMOJI[i].name, name) == 0) return EMOJI[i].emoji;
    return NULL;
}

/* Case-insensitive "does `s` start with `pre`". An empty prefix matches. */
static int ci_prefix(const char *s, const char *pre) {
    for (; *pre; s++, pre++)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    return 1;
}

/* Case-insensitive "does any space-separated word of `hay` start with `pre`".
 * Word-prefix rather than substring so "art" finds :art: but not :heart:, which
 * is the ordering people expect from a shortcode search. */
static int ci_word_prefix(const char *hay, const char *pre) {
    if (!hay || !hay[0]) return 0;
    for (const char *p = hay; *p; ) {
        if (ci_prefix(p, pre)) return 1;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }
    return 0;
}

size_t oc_emoji_search(const char *query, const oc_emoji **out, size_t max) {
    size_t n = 0, total = sizeof EMOJI / sizeof *EMOJI;
    for (size_t i = 0; i < total && n < max; i++) {
        if (!query || !query[0] ||
            ci_prefix(EMOJI[i].name, query) ||
            ci_word_prefix(EMOJI[i].keywords, query))
            out[n++] = &EMOJI[i];
    }
    return n;
}

/* ---- completion ----------------------------------------------------------- */

/* Substring, case-insensitively — the second band of oc_complete_targets. */
static int ci_contains(const char *s, const char *needle) {
    if (!s || !needle || !*needle) return 1;
    size_t nl = strlen(needle);
    for (const char *p = s; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return 1;
    }
    return 0;
}

size_t oc_complete_targets(const oc_model *m, const char *query,
                           oc_target *out, size_t max) {
    if (!m || !out || max == 0) return 0;
    const char *q = query ? query : "";
    /* A leading sigil is accepted and ignored rather than treated as a filter:
     * somebody typing "@ali" in a To: field means the person, not a literal. */
    if (*q == '@' || *q == '#') q++;
    size_t n = 0;
    /* Two passes so prefix matches lead, and within each pass people lead —
     * you address a person more often than a channel. */
    for (int band = 0; band < 2 && n < max; band++) {
        for (size_t i = 0; i < m->n_users && n < max; i++) {
            const char *nm = m->users[i].name;
            if (!nm || !nm[0] || m->users[i].user_id == m->user_id) continue;  /* not yourself */
            int pre = ci_prefix(nm, q);
            if (band == 0 ? !pre : (pre || !ci_contains(nm, q))) continue;
            out[n].id = m->users[i].user_id;
            out[n].is_channel = 0;
            snprintf(out[n].name, sizeof out[n].name, "%s", nm);
            /* The subtitle is their title if they set one — Slack shows the real
             * name beside the handle; ours has a title field and no second name. */
            snprintf(out[n].sub, sizeof out[n].sub, "%s", m->users[i].title);
            n++;
        }
        for (size_t i = 0; i < m->n_channels && n < max; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->kind == OC_CHANNEL_KIND_DM || !c->name || !c->name[0]) continue;
            int pre = ci_prefix(c->name, q);
            if (band == 0 ? !pre : (pre || !ci_contains(c->name, q))) continue;
            out[n].id = c->channel_id;
            out[n].is_channel = 1;
            snprintf(out[n].name, sizeof out[n].name, "%s", c->name);
            out[n].sub[0] = '\0';
            n++;
        }
    }
    return n;
}

size_t oc_complete(const oc_model *m, const char *text,
                   oc_completion *out, size_t max, int *repl_start, int *kind) {
    if (kind) *kind = OC_AC_NONE;
    if (repl_start) *repl_start = 0;
    if (!m || !text || !out || max == 0) return 0;

    /* The trailing token: everything back to the last whitespace. */
    size_t len = strlen(text);
    int ws = 0;
    for (int i = (int)len - 1; i >= 0; i--)
        if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r') { ws = i + 1; break; }
    const char *tok = text + ws;
    if (repl_start) *repl_start = ws;

    size_t n = 0;

    if (tok[0] == ':' && !strchr(tok + 1, ':')) {
        if (kind) *kind = OC_AC_EMOJI;
        const oc_emoji *hits[64];
        size_t nh = oc_emoji_search(tok + 1, hits, 64);
        for (size_t i = 0; i < nh && n < max; i++) {
            snprintf(out[n].repl, sizeof out[n].repl, "%s", hits[i]->emoji);
            snprintf(out[n].disp, sizeof out[n].disp, "%s  :%s:", hits[i]->emoji, hits[i]->name);
            n++;
        }
        return n;
    }

    if (tok[0] == '@') {
        if (kind) *kind = OC_AC_MENTION;
        for (size_t i = 0; i < m->n_users && n < max; i++)
            if (m->users[i].name[0] && ci_prefix(m->users[i].name, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "@%s", m->users[i].name);
                snprintf(out[n].disp, sizeof out[n].disp, "%s", m->users[i].name);
                n++;
            }
        return n;
    }

    if (tok[0] == '#') {
        if (kind) *kind = OC_AC_CHANNEL;
        for (size_t i = 0; i < m->n_channels && n < max; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->kind != OC_CHANNEL_KIND_DM && c->name && ci_prefix(c->name, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "#%s", c->name);
                snprintf(out[n].disp, sizeof out[n].disp, "%s", c->name);
                n++;
            }
        }
        return n;
    }

    return 0;
}
