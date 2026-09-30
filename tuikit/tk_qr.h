/*
 * tuikit — tk_qr: a QR code as terminal text (AUTH.md §8.11's device code, for a
 * phone's camera). Two modules to a character cell, in half blocks, with a
 * quiet zone around. Pure: no terminal, so it can be tested; the caller draws
 * the rows with LIGHT modules as the foreground on a dark background, which
 * reads the same on a light terminal or a dark one.
 *
 * The encoding is Nayuki's qrcodegen (third_party/qrcodegen, MIT).
 */
#ifndef TK_QR_H
#define TK_QR_H

#include <stddef.h>

/* The QR code of `text` (low error correction, the smallest version that holds
 * it) as `*rows` lines of `*cols` cells each, joined by '\n' into `out`: '█' both
 * modules light, '▀' the upper one, '▄' the lower one, ' ' neither -- UTF-8,
 * so up to three bytes a cell. 0, or -1 if it does not encode or fit. */
int tk_qr_render(const char *text, char *out, size_t cap, int *cols, int *rows);

#endif /* TK_QR_H */
