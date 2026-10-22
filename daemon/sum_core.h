/*
 * OpenChime — channel and DM summaries: the engine-free core (REQ-310, ARCH-116,
 * docs/SUMMARIES.md).
 *
 * Two steps, both on numbered lines that stand for the messages behind them:
 *   - notes: lines in, notes out in proportion to how much was read
 *     (oc_sum_notes_for) and always fewer than the lines, so every merge
 *     level is smaller by its structure, not by the model's choice;
 *   - the summary a reader is shown: the shape of the standard (SUMMARIES.md
 *     §1), sized to the span, written from messages when they fit one prompt,
 *     else from the notes alone.
 * The model writes inside a grammar that bounds how many lines of each part
 * there are and how long each line is; code checks what it wrote (citations,
 * numbers, people, repeats) and holds each part and the whole to its cap.
 * This file is everything about that which needs neither the database nor a
 * model. Pure C99: the tests drive it without a model.
 */
#ifndef OC_SUM_CORE_H
#define OC_SUM_CORE_H

#include <stddef.h>
#include <stdint.h>

/* The size of a leaf chunk (the one constant tuned by evaluation) and the
 * quiet gap that starts a new chunk. */
#ifndef SUM_THRESHOLD_TOKENS
#define SUM_THRESHOLD_TOKENS 1500
#endif
#ifndef SUM_GAP_MS
#define SUM_GAP_MS (45ull * 60ull * 1000ull)
#endif
/* The one size estimate every backend shares, so where pieces are cut never
 * depends on the model: a token per this many bytes of text. */
#define SUM_BYTES_PER_TOKEN 4
/* The model's context, and the most of it one call's lines may take: the rest
 * holds the instructions and the answer (SUMMARIES.md §2). */
#define SUM_CTX_TOKENS   8192
#define SUM_INPUT_TOKENS 6000
/* Notes: one per SUM_TOKENS_PER_NOTE tokens read (the tunable ratio), at
 * least SUM_NOTES_MIN, at most what the answer room holds -- and at most half
 * the lines read, which is what ends the recursion, though never fewer than
 * SUM_NOTES_FLOOR when there are that many lines: a few unrelated lines are
 * not forced into one note. Each cites at most SUM_CITES_MAX lines, its fact
 * at most SUM_FACT_CHARS, its topic SUM_TAG_CHARS. The answer's rail leaves
 * room for SUM_NOTES_SLACK notes past what was asked: the parser drops them,
 * not the call. */
#define SUM_TOKENS_PER_NOTE 250
#define SUM_NOTES_MIN       4
#define SUM_NOTE_TOKENS     128
#define SUM_NOTES_FLOOR     3
#define SUM_NOTES_SLACK     2
#define SUM_NOTES_ROOM      ((SUM_CTX_TOKENS - SUM_INPUT_TOKENS) / SUM_NOTE_TOKENS)
#define SUM_CITES_MAX       3
#define SUM_FACT_CHARS      320
#define SUM_TAG_CHARS       40
/* The standard (SUMMARIES.md §1): a summary of about a word to every
 * SUM_WORDS_PER_WORD of its span, between SUM_MIN_WORDS and SUM_MAX_WORDS and
 * never longer than the span. Its size is asked for as shape, not as a word
 * count a model cannot hit: a topic to every hundred of those words, between 2
 * and SUM_MAX_TOPICS, at least all but two of them written, with at least two
 * details each from 300 words up. The parts' caps are bounds on a runaway
 * line, not a length to write to. */
#define SUM_WORDS_PER_WORD  10
#define SUM_MIN_WORDS       100
#define SUM_MAX_WORDS       600
#define SUM_OVERVIEW_WORDS  60
#define SUM_PARA_WORDS      80
#define SUM_DETAIL_WORDS    40
#define SUM_MAX_TOPICS      6
#define SUM_FULL_DETAILS    3
#define SUM_FULL_ATTENTION  5
#define SUM_MORE_TOPICS     10
/* Answer room, in tokens: a rail above what the shape can hold, never the
 * length control (SUMMARIES.md §2). SUM_TOKENS_PER_WORD to a word of the
 * shape's room -- measured at 1.9 to 4.4 tokens to a kept word across models,
 * the citations and headings included -- and spare on top. An answer that
 * reaches it is a runaway. */
#define SUM_TOKENS_PER_WORD    4
#define SUM_FINAL_SPARE_TOKENS 200
/* A summary over its total by more than this much, in percent, is handed back
 * to the model once to be shortened before the trim. */
#define SUM_SHORTEN_OVER_PCT     20
/* Rewrites of one part over its cap before it is cut at a sentence. */
#define SUM_REWRITES             2
/* Bumped whenever the prompt, the shape or how an answer is read change. */
#define SUM_PROMPT_VERSION "s15"

/* One message as the core sees it: plain text (oc_speakable_full), its author's
 * name, and its thread (parent_id: the root's id, 0 for a top-level message). */
typedef struct {
    int64_t     id, parent_id, author_id;
    int64_t     created_ms, edited_ms;
    const char *author;
    const char *text;
} oc_sum_msg;

/* A piece of a channel to summarize: the indices (into the caller's message
 * array, in order) of the messages it holds. `root_id` is the thread's root when
 * the piece is all or part of one thread. `end_ms` is its last activity. */
typedef struct {
    int    *idx;
    int     n;
    int64_t root_id;
    int64_t end_ms;
} oc_sum_chunk;

/* A unit of the cut: one chunk, or a thread too big for one, held as chunks of
 * its own (summarized, and the summaries summarized, to one thread summary). */
typedef struct {
    oc_sum_chunk *chunks;
    int           n_chunks;
    int           is_big_thread;
    int64_t       root_id;
    int64_t       end_ms;
} oc_sum_piece;

typedef struct {
    oc_sum_piece *pieces;
    int           n;
} oc_sum_cut;

/* The estimated tokens of one message as a line. */
size_t oc_sum_msg_tokens(const oc_sum_msg *m);

/* Cut `msgs` (n, sorted by id; replies carry their root's id in parent_id, and a
 * thread's root must be present when any reply is) into pieces, in order of each
 * piece's last activity. A message larger than `threshold` is a chunk of its
 * own. `threshold` and `gap_ms` are the two constants, passed so the tests and
 * the evaluation tool can vary them. 0, or -1 out of memory. */
int  oc_sum_cut_build(const oc_sum_msg *msgs, int n, size_t threshold, uint64_t gap_ms, oc_sum_cut *out);
void oc_sum_cut_free(oc_sum_cut *c);

/* A growing string. */
typedef struct { char *p; size_t n, cap; int oom; } oc_sum_buf;
void oc_sum_buf_add(oc_sum_buf *b, const char *s, size_t n);
void oc_sum_buf_puts(oc_sum_buf *b, const char *s);
void oc_sum_buf_printf(oc_sum_buf *b, const char *fmt, ...);
/* `s` as a JSON string, quotes included. */
void oc_sum_buf_json(oc_sum_buf *b, const char *s);
void oc_sum_buf_free(oc_sum_buf *b);

/* The input of one step: numbered lines. A line stands for messages: `refs`
 * the ones to show for it (at most SUM_CITES_MAX), `all` every one behind it;
 * `by` their authors, one to a line; `kind` a note's (e, d, a, q) or 0 for a
 * message. `label`, when set, is printed above the line (the date of what
 * follows). */
typedef struct {
    char    *text;
    int      indent;          /* a reply in a thread */
    char    *label;
    int64_t *refs;
    int      n_refs;
    int64_t *all;
    int      n_all;
    char    *by;
    char     kind;
} oc_sum_line;
typedef struct {
    oc_sum_line *v;
    int          n, cap;
} oc_sum_lines;
/* Append a line (copies everything). 0 or -1. The short form stands for its
 * refs, by no one in particular. */
int  oc_sum_lines_add(oc_sum_lines *l, const char *text, int indent, const char *label,
                      const int64_t *refs, int n_refs);
int  oc_sum_lines_add_full(oc_sum_lines *l, const char *text, int indent, const char *label,
                           const int64_t *refs, int n_refs, const int64_t *all, int n_all,
                           const char *by, char kind);
void oc_sum_lines_free(oc_sum_lines *l);
/* The estimated tokens the lines render to, and the words in them. */
size_t oc_sum_lines_tokens(const oc_sum_lines *l);
size_t oc_sum_lines_words(const oc_sum_lines *l);

/* The lines of a chunk of messages: "Name: text", replies indented. A run of
 * messages that say the same but for their numbers, whoever wrote them, is one
 * line with how many: "(x14)". 0 or -1. */
int oc_sum_chunk_lines(const oc_sum_msg *msgs, const oc_sum_chunk *c, oc_sum_lines *l);

/* Split `text` into parts of at most `max_tokens` each: at paragraphs, then
 * lines, then sentences, then words, and only as a last resort mid-word (never
 * mid-character). Every byte of the text is in exactly one part. *parts (heap
 * array of heap strings) and their count. 0 or -1. */
int  oc_sum_split_text(const char *text, size_t max_tokens, char ***parts, int *n);
void oc_sum_parts_free(char **parts, int n);

/* The system prompt: the same for every call. */
extern const char *const OC_SUM_SYSTEM;

/* --- the prompts' words ---------------------------------------------------------
 * Each prompt is built from these, so the words are in one place and a test
 * checks a prompt by building its parts from the same constants rather than
 * by quoting them. The labels are also what the answer's grammar fixes and
 * what reading an answer looks for. */
#define SUM_LBL_OVERVIEW  "Overview:"
#define SUM_LBL_ATTENTION "Needs attention:"
#define SUM_LBL_MORE      "More topics:"
/* What each stage's prompt opens with (the intro), by the worker. */
#define SUM_INTRO_MESSAGES "Messages from %s, oldest first. Indented lines are replies in a thread."
#define SUM_INTRO_NOTES    "Notes on consecutive parts of %s, oldest first. Each is: (kind) topic: what happened."
#define SUM_INTRO_PART     "One long message in %s, part %d of %d."
/* Notes: the ask, after the lines; its %d are notes_max and SUM_CITES_MAX. */
#define SUM_NOTES_FORM "- [line number] kind | topic | what happened"
#define SUM_NOTES_ASK                                                                                   \
    "\nWrite notes on these lines: at most %d, the most important first. Each note is one line in "      \
    "exactly this form:\n" SUM_NOTES_FORM "\n"                                                          \
    "\n"                                                                                                \
    "kind is event, decision, action or question. topic names what the note is about in 2 to 5 "        \
    "words, and notes about the same thing share it. what happened is one or two sentences saying who " \
    "did what, with the particulars the lines give. Put what is about the same thing into one note, "   \
    "citing the lines it comes from (at most %d), each line number in square brackets, as [12] or "     \
    "[12][15]. Leave out greetings and small talk.\n"
/* The summary: the form, after the lines; the "More topics" line of it when
 * there may be more; the ask (%d: topics_min, topics, details_min, details,
 * attention); the more's ask (%d: more); the citation rule (%d: SUM_CITES_MAX). */
#define SUM_FINAL_FORM                                                                     \
    "\nSummarize these lines for someone catching up, in exactly this form:\n"             \
    "\n"                                                                                   \
    SUM_LBL_OVERVIEW " [line number] <the most important outcome or change>\n"             \
    "## <what a topic is about>\n"                                                         \
    "[line number] <what happened in it, and where it stands now>\n"                       \
    "- [line number] <one detail: who did or said what>\n"                                 \
    SUM_LBL_ATTENTION "\n"                                                                 \
    "- [line number] action: <who has to do what, and by when if it was said>\n"           \
    "- [line number] question: <a question that was asked and not answered>\n"
#define SUM_FINAL_FORM_MORE SUM_LBL_MORE " <topic>; <topic>\n"
#define SUM_FINAL_ASK                                                                                     \
    "\nWrite %d to %d topics, the most important first, each titled in 2 to 6 words, with what happened "  \
    "in it in one to three sentences and %d to %d details of one sentence each. The overview is one "      \
    "sentence. Write \"Needs attention\" only for actions and unanswered questions, at most %d, one "      \
    "sentence each, and leave it out when there are none."
#define SUM_FINAL_ASK_MORE " Name any further topics, at most %d, after \"More topics\"."
#define SUM_FINAL_CITE                                                                                     \
    " Every line starts with the numbers of the lines it comes from, at most %d, "                         \
    "each in square brackets, as [12] or [12][15]. Write only what the lines say, with their particulars, " \
    "and name a person only for what their own lines say. Do not comment on, interpret or characterize "   \
    "what happened.\n"
/* A part's rewrite (%zu: its words, %s: it, %d: the cap), and a whole
 * summary's (the head's %d its words; the ask's %d the total). */
#define SUM_REWRITE_ASK                                                                                  \
    "This is %zu words:\n\n%s\n\nRewrite it in at most %d words. Keep every name, number and fact "       \
    "that fits, and add nothing.\n"
#define SUM_SHORTEN_HEAD "This summary is %d words:\n\n"
#define SUM_SHORTEN_ASK                                                                                     \
    "\nWrite it again in at most %d words, in exactly the same form, each line keeping the line numbers it " \
    "starts with. Keep every name, number and fact that fits, the most important first, and add nothing.\n"

/* --- notes --- */
/* How many notes one call may write on `input_tokens` of `input_lines` lines. */
int oc_sum_notes_for(size_t input_tokens, int input_lines);
/* The prompt and grammar for notes on `l`: `intro` (what the lines are), the
 * numbered lines, and at most `notes_max` notes, each "- [n] kind | topic |
 * fact". 0 or -1. */
int oc_sum_notes_prompt(const char *intro, const oc_sum_lines *l, int notes_max, oc_sum_buf *prompt,
                        oc_sum_buf *grammar);
/* Read notes back into a stored body:
 *   {"notes":[{"kind":"event|decision|action|question","topic":"...","text":"...",
 *              "refs":[...],"all":[...],"by":["name",...]}]}
 * A note is dropped when it cites nothing, says nothing, repeats one kept, or
 * when every sentence states a number or names a person its lines do not; a
 * sentence that does is taken out. The first `notes_max` kept are taken (0:
 * all): a model that writes past what it was asked for loses the rest, not
 * the call. How many kept, or -1. */
int oc_sum_parse_notes(const char *answer, const oc_sum_lines *l, oc_sum_buf *out, int *dropped, int notes_max);
/* Append a notes body's lines: "(kind) topic: fact", each standing for what the
 * note does. `label` above the first. 0, or -1 (out of memory, not notes). */
int oc_sum_notes_lines(const char *body, const char *label, oc_sum_lines *l);

/* --- the summary a reader is shown --- */
typedef struct {
    int overview, para, detail;      /* words, per part */
    int topics_min, topics, details_min, details, attention, more;
    int total;                       /* words in all: what is asked for and trimmed to */
    int room;                        /* words the shape can hold at most: what the answer's rail is sized to */
} oc_sum_caps;
/* The caps for a span of `input_words`, sized to it (SUMMARIES.md §1). */
void oc_sum_caps_for(size_t input_words, oc_sum_caps *c);

typedef struct {
    char    *text;                   /* heap */
    int64_t  refs[SUM_CITES_MAX];    /* the messages it shows */
    int      n_refs;
    int     *cited;                  /* the lines it cites, heap */
    int      n_cited;
} oc_sum_part;
typedef struct {
    char        *title;
    oc_sum_part  para;
    oc_sum_part  det[SUM_FULL_DETAILS];
    int          nd;
    int          weight;             /* 3 a decision or action, 2 a question, 1 */
    int64_t     *all;                /* every message behind it */
    int          n_all;
    char        *by;                 /* their authors, one to a line */
    int64_t      last;
} oc_sum_topic;
typedef struct {
    oc_sum_part  ov;
    oc_sum_topic top[SUM_MAX_TOPICS];
    int          nt;
    oc_sum_part  att[SUM_FULL_ATTENTION];
    char         att_kind[SUM_FULL_ATTENTION];   /* 'a' action, 'q' question */
    int          na;
    char        *more[SUM_MORE_TOPICS];
    int          nm;
    size_t       words;              /* the words of what it summarizes */
} oc_sum_final;

/* The prompt and grammar for the summary of `l` under `c`. 0 or -1. */
int  oc_sum_final_prompt(const char *intro, const oc_sum_lines *l, const oc_sum_caps *c, oc_sum_buf *prompt,
                         oc_sum_buf *grammar);
/* Read an answer back into `f`, checking each part as notes are checked. A
 * topic left empty is none; no overview: the first topic's account stands in.
 * 0, or -1 when nothing is left (or out of memory). */
int  oc_sum_parse_final(const char *answer, const oc_sum_lines *l, const oc_sum_caps *c, oc_sum_final *f,
                        int *dropped);
/* Check a part's text (a rewrite) against the lines it cites, as parsing does.
 * How many sentences were taken out. */
int  oc_sum_part_check(oc_sum_part *p, const oc_sum_lines *l);
/* Topics by weight, then how many messages, then how recent. */
void oc_sum_final_rank(oc_sum_final *f);
/* Hold the whole to its cap: the lowest topics to "More topics", then the last
 * details and attention items, then the account and the overview cut at a
 * sentence. 1 when anything changed. */
int  oc_sum_final_fit(oc_sum_final *f, const oc_sum_caps *c);
/* Cut `text` to `max_words`: at the last whole sentence that fits, else at a
 * word. 1 when it was cut. */
int  oc_sum_cut_words(char *text, int max_words);
/* The stored body:
 *   {"overview":{"text","refs"},
 *    "topics":[{"title","text","refs","count","people":[names],"details":[{"text","refs"}]}],
 *    "attention":[{"kind":"action|question","text","refs"}],"more":["title",...],
 *    "words":<the words of what it summarizes, which chose its size>} */
int  oc_sum_final_json(const oc_sum_final *f, oc_sum_buf *out);
void oc_sum_final_free(oc_sum_final *f);
/* A rewrite of one part to `cap` words. 0 or -1. */
int  oc_sum_rewrite_prompt(const char *text, int cap, oc_sum_buf *prompt, oc_sum_buf *grammar);
/* The words of a parsed summary, as the trim counts them (titles included,
 * "More topics" not). */
int  oc_sum_final_words(const oc_sum_final *f);
/* `f` written back in the answer's own form, line numbers and all. 0 or -1. */
int  oc_sum_final_text(const oc_sum_final *f, oc_sum_buf *out);
/* The whole summary `f`, over `c->total`, handed back to be written again in
 * at most that many words, in the same form (the final's grammar holds it).
 * 0 or -1. */
int  oc_sum_shorten_prompt(const oc_sum_final *f, const oc_sum_caps *c, oc_sum_buf *prompt);

/* 1 when a stored body is a summary of the shape this version writes. */
int oc_sum_body_readable(const char *body);
/* 1 when a stored body (notes or a summary) says nothing. */
int oc_sum_body_empty(const char *body);

/* Words in `s` (runs of non-space). */
size_t oc_sum_words(const char *s);

#endif
