/* A small tokenizer for snippets (REQ-226): enough to colour keywords,
 * strings, comments and numbers in eight languages -- C, Python, JavaScript,
 * shell, JSON, YAML, SQL and Go -- the same in every client. It is not a
 * parser: it reads one pass, left to right, and anything it does not know is
 * plain. An unknown language is all plain.
 *
 * Built into shared/richtext.c, beside the rest of the text rendering rules. */
#ifndef OPENCHIME_LEXER_H
#define OPENCHIME_LEXER_H

#include <stddef.h>
#include <stdint.h>

enum { OC_LANG_NONE = 0, OC_LANG_C, OC_LANG_PYTHON, OC_LANG_JS, OC_LANG_SHELL,
       OC_LANG_JSON, OC_LANG_YAML, OC_LANG_SQL, OC_LANG_GO, OC_LANG_COUNT };

enum { OC_LEX_PLAIN = 0, OC_LEX_KEYWORD, OC_LEX_STRING, OC_LEX_COMMENT, OC_LEX_NUMBER };

/* One coloured span: `len` bytes from `start`. Spans are in order and never
 * overlap; the bytes between them are plain. */
typedef struct { uint32_t start, len; uint8_t kind; } oc_lex_tok;

/* The language a name means -- its own ("c", "python", "javascript", "shell",
 * "json", "yaml", "sql", "go") or a common other ("h", "py", "js", "ts", "sh",
 * "bash", "yml", "golang"), any case -- or OC_LANG_NONE. */
int oc_lex_language(const char *name);
/* A language's own name, as a snippet stores it ("" for none). */
const char *oc_lex_name(int lang);
/* Its name for people ("C", "Python", ...; "Plain text" for none). */
const char *oc_lex_label(int lang);

/* Colour `n` bytes of `s` in `lang`: up to `cap` spans into `out`, and the
 * count. A full `out` ends the pass: what follows is plain. */
size_t oc_lex(int lang, const char *s, size_t n, oc_lex_tok *out, size_t cap);

#endif
