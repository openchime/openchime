# Image previews

The design of the daemon's image previews: what is made, when an image gets
none, how each format is handled, the four bounds every decode is held to and
their values, how the decoders are fuzzed, and how a pasted image is named. It
delivers REQ-390 to REQ-393 and amends REQ-142; the decision and what it
rejected are ARCH-137. Built: `daemon/preview.c`, `OC_XFER_PREVIEW` in
`daemon/xferpool.c`, migration 0088 (SCHEMA.md §3ca), the `PREVIEW_*` frames
(PROTOCOL.md §5.14f) and the clients' rung choice (CLIENT.md, "Images: a preview
at the size drawn").

## 1. What is made, and when

When an upload is confirmed (`UPLOAD_OK`) and the uploader called it an image
(`image/*`), the network loop hands its storage key to a **transfer worker**,
which reads the original from the store, makes the previews and stores them. The
uploader's type only decides whether to look: the worker goes by the file's own
magic bytes (`oc_preview_sniff`). Nothing of this reaches the uploader, and the
message is sent before any preview exists; a reader who asks in that moment gets
`PREVIEW_MISSING` and the original.

Two **rungs**, each the original scaled down with its aspect kept so its longer
side is at most the rung's box, never scaled up:

| Rung | Wire | Box (default) | Setting | Drawn |
|---|---|---|---|---|
| inline | `1` | 480 px | `OPENCHIME_PREVIEW_INLINE_PX` | in the timeline, inside 260 × 160 points |
| expanded | `2` | 1600 px | `OPENCHIME_PREVIEW_EXPANDED_PX` | in the image viewer, up to the window |

**Why two, not one or seven.** One rung serves either the timeline badly (a
viewer-sized image to draw a thumbnail) or the viewer badly (a thumbnail blown
up). Seven, the shape of a shared service's ladder, costs generation time and
storage on every upload for sizes nobody on this client asks for, on a box where
storage is already what runs out. The two sizes cover the timeline at up to
twice its points on a high-density screen, and the viewer on a large monitor.
The original is always kept and is what Save writes.

**Both rungs or none.** An image larger than the inline box gets both, even when
it already fits the expanded box: re-encoded at its own size, the expanded rung
is still smaller than an original straight off a camera, and a client never has
to ask which rungs exist.

## 2. The no-preview-needed predicate

An image gets **no preview** when both hold:

- its width and its height are each at most the inline box (480 px by default);
- the file is at most **256 KiB** (`OC_PREVIEW_SMALL_BYTES`).

That is memes, icons, emoji screenshots and small diagrams: the original is as
cheap to send as a preview would be, so none is generated or stored, and a
client that asks gets `PREVIEW_MISSING` and the original. An image within the
box but heavier than 256 KiB (an uncompressed PNG, say) does get previews, since
re-encoding it saves bytes.

## 3. Per format

| Format | Recognised by | Decoded with | Preview |
|---|---|---|---|
| JPEG | `FF D8 FF` | libjpeg-turbo, libjpeg API | JPEG, quality 82, optimised Huffman tables; scaled by libjpeg's own 1/2, 1/4 or 1/8 while decoding where the result still covers the expanded box |
| PNG | the 8-byte signature | libspng, miniz as its zlib | PNG; RGBA kept when the image has alpha (an alpha colour type, or `tRNS`), RGB otherwise |
| WebP | `RIFF`…`WEBP` | libwebp, incrementally | WebP: lossless in, lossless out (VP8L); lossy in, lossy out at quality 80; decoded straight to the expanded box by libwebp's own scaler |
| GIF | `GIF87a`/`GIF89a` | — | none: shown as itself, the client capping its size; a still of an animation is worse than the animation |
| HEIC | an `ftyp` box with a HEIF brand | — | none: no decoder is linked, by choice (ARCH-137); the client offers the file |
| animated PNG | an `acTL` chunk before the first `IDAT` | — | none: libspng would read the first frame only |
| animated WebP | `WebPGetFeatures`' animation flag | — | none, as GIF |
| CMYK or YCCK JPEG | its colour space | — | none: print files, left to their original |
| anything else | — | — | none |

The reduction is **area averaging**: each output pixel is the coverage-weighted
mean of the source pixels under it, which is the right filter for shrinking (no
aliasing, no ringing), and alpha is averaged premultiplied so a transparent
pixel's colour does not bleed into an opaque edge. The inline rung is made from
the expanded one. EXIF orientation is not applied, because the clients draw the
original without it: a preview looks as its original does.

## 4. The four bounds

This is the first place the daemon parses untrusted media, and anyone who can
post in a channel feeds it. Every decode is held to all four; none can be turned
off, though an operator can move the values.

| Bound | Default | Setting | Where it is checked | Stops |
|---|---|---|---|---|
| hardened decoders only | libjpeg-turbo, libspng, libwebp | — | the build (VENDORS.md §1) | the decoders with a poor record on malformed input, stb_image above all |
| byte cap | 32 MiB | `OPENCHIME_PREVIEW_MAX_BYTES` | the store's size for the blob, **before a byte is read** | a decode costing time and memory in proportion to a huge file |
| pixel cap | 50 megapixels | `OPENCHIME_PREVIEW_MAX_PIXELS` | width × height **from the header, before any pixel buffer is allocated**: the JPEG's frame header, the PNG's `IHDR`, the WebP's `VP8`/`VP8L`/`VP8X` header | decompression bombs: a few hundred bytes claiming gigabytes of pixels |
| time limit | 5 s | `OPENCHIME_PREVIEW_TIMEOUT_MS` | every row of the decode, resize and encode; libjpeg's progress monitor inside its own passes; between 64 KiB slices of a WebP; libwebp's encoder progress hook | a pathological file that is small and slow (a progressive JPEG of thousands of scans, say) |

libspng's own limits sit under ours: no chunk over 8 MiB, no more than 16 MiB of
ancillary chunks cached. A JPEG decode stops on libjpeg's first corrupt-data
warning rather than recovering from it: a preview of a damaged file is not worth
making.

An image refused by any bound, or that does not decode, gets **no preview**: it
is served whole, the uploader is told nothing, and the daemon logs one line
naming the attachment and the reason (`no preview (over the pixel cap)`).
Memory while a preview is made is about four bytes a pixel of the decoded
image, at most one image per transfer worker at once; the pixel cap is what
bounds it.

## 5. Fuzzing

The three decoders, and the resize and encoders behind them, are in the
fixed-seed sweep every `make test` runs (`tests/test_fuzz.c`,
`test_fuzz_images`): a valid JPEG, PNG, lossy WebP and lossless WebP, each
mutated 400 times — bytes flipped, runs overwritten, the file cut short — and
made into previews under boxes small enough that each is decoded rather than
passed as small. Every result must be one of the statuses, a made preview must
probe as an image, and a refused one must hold no rung. The bounds have their own tests
(`tests/test_preview.c`): a byte cap one under the file, PNG and JPEG headers
claiming 60 000 and 65 000 pixels a side, and the time limit on an injected
clock that moves a second each time it is read.

They are also a harness of the coverage-guided tier (docs/details/TESTING.md
§2.2b): `tests/fuzz/image.c`, which makes previews of its input under the
same small boxes and holds the same contract, with a corpus of a small JPEG,
PNG with and without alpha, and lossy and lossless WebP
(`tests/fuzz/corpus/image/`). `make test` replays the corpus; the full suite
fuzzes from it under ASan and UBSan. The vendored decoders are linked as the
daemon builds them, so the fuzzer's coverage is of `daemon/preview.c`; the
sanitizers still watch every byte the decoders touch.

## 6. Storage

Each rung is a blob under the original's key followed by `-1` or `-2`, which no
other blob's key can be (an attachment's key is hex alone), with a row in
`attachment_previews` (SCHEMA.md §3ca). A rung is **derived data**: under storage
pressure previews go before any attachment, least recently served first, with
no tombstone; and, pressure or not, a preview goes when its attachment's bytes
do. Previews finished after their attachment went are not recorded, and their
blobs are deleted at once.

## 7. A pasted image's name

An image pasted from the clipboard (CLIENT.md, "Images: a preview at the size
drawn") is uploaded under `pasted-image-YYYYMMDD-HHMMSS.<ext>`, the local time
it was pasted and the extension of the format the clipboard gave it (`gif` for a GIF
on Windows and `png` otherwise; on the web the browser's type, `png` when it
gives none), held in memory and never written to
disk. Its previews are made from it as from any upload.
