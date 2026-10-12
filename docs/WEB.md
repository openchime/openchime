# The web client

How the graphical client comes to run in a browser (ARCH-74, CLIENT.md §3
"Web"), what of it exists, and what is left, in the order it is built. The
end state is the one portable GUI (ARCH-80) compiled to WebAssembly with
Emscripten, served by the daemon from its own origin, carrying the protocol
inside WebSocket frames. The application is the same sources as the Windows
client (`client/gui/app/`); nothing here is a second implementation of its
views.

## What exists

**The transport.** The daemon speaks the protocol over a WebSocket
(PROTOCOL.md §1.2): `GET /ws` with the upgrade headers, on the TLS port
beside the webhook and sign-in pages, and on the plaintext health port for a
page served from the same box in development. After the `101` the connection
is a protocol peer exactly as one that negotiated the `oc/1` ALPN: each
WebSocket binary frame's payload joins the byte stream the frame buffer reads,
each send goes out as one binary frame, a ping is answered, a close or a text
frame ends it. It is the I/O thread's (`daemon/ioloop.c`, ARCH-22); the event
loop hears `OPENED` a second time, as a protocol peer, and nothing else in it
changed. Tested in `tests/itest_netloop.c` (the upgrade, the HELLO's answer,
ping, an unmasked frame).

**The core in a browser.** `client/core/*.c` and the shared wire compile
with Emscripten, with pthreads: the network thread is a Web Worker and its
socket is Emscripten's WebSocket, which the page points at `/ws`.
Inside the WebSocket the protocol goes in the clear: the page's HTTPS is the
wire's TLS, so the core opens a **plain connection** (`oc_tls_conn_init_plain`)
and judges no certificate -- the browser judged the origin's. mbedTLS still
links, for the sign-in hashing and the call keys, built for wasm once beside
the native build (`scripts/build/mbedtls_wasm.sh`). Three places in the core
have an Emscripten branch and no more: the connection's setup (`net.c`), SRV
lookup (`resolve.c`, a browser has no resolver), and `poll` (`sock.h`,
Emscripten's reports and never waits, so a timeout is a loop of short sleeps
on the network thread). The credential store is in memory for now
(`client/gui/platform/web/secret_mem.c`).

**The drawing layer in a browser.** SDL3 builds for wasm from the same release
the Windows client uses (`scripts/build/sdl3_wasm.sh`), and `oc_gfx` compiles
over it unchanged. `make web-test` runs the backend's own pixel test
(`client/gui/gfx/gfx_test_win.c`, the same assertions) under Node first.

**The application in a browser.** `make web` compiles the application
layer -- `client/gui/app/`, the same sources `make windows` compiles --
over the browser's platform directory (`client/gui/platform/web/`). The
window, its events, timers, cursors and clipboard text are SDL3 on both; what
only a platform can do is the contract in `client/gui/platform/platform.h`,
and the browser's implementation (`plat_web.c`) answers "not available" for
what a page cannot do yet (a tray, a file picker, a crash dump, a second
process to hand a URL to) and does what it can (the badge is the tab's
title). The application's loop yields to the browser between turns (Asyncify).
Text is the canvas backend of sdltext (`sdltext/st_canvas.c`): the browser
measures and rasterizes runs, the backend lays out lines, styles ranges,
places inline boxes and hit-tests on the byte map `st_common.c` keeps, and
pixels leave through the same sink as DirectWrite's; measurements are cached
by font and text, and a layout keeps its last raster, so a frame of the
conversation view costs about ten milliseconds in a headless Chromium. The
text fields are drawn by the application on every platform (CLIENT.md, "The
text fields are drawn"), so the sign-in field, the search boxes and a form's
fields are the same code here. Signed in with a password, it
shows the workspace: the sidebar, a channel's transcript with mentions, the
member pane, the composer; `make web-test` checks that through the client's
own test hook (the model dumped, a screenshot taken) in a headless Chromium.

**The browser platform** (`client/gui/platform/web/`), what a page can do of
the contract: images decode through `createImageBitmap` and a 2D canvas, read
back premultiplied as the renderer wants them (Asyncify lets the
application's synchronous call wait on the page's promise); the credential
store is memory with `localStorage` behind it, so a reload signs in again
from the stored token (`secret_mem.c`; the core writes a token from its
network thread, which hands the write to the page's thread); notifications
are the Notification API (`client/shared/osnotify_web.c`): a tag per
conversation so a second replaces the first and one can be withdrawn, a click
focuses the page and hands the notification's URL to the application as a
second launch would; the file picker is an `<input type=file>` whose files
are copied onto the page's filesystem and answered as paths; a download is
written to the page's filesystem and then downloaded by the browser
(`oc_plat_file_saved`); a dropped file arrives through SDL's drop event; an
image on the clipboard comes through the async clipboard API; the badge is
the tab's title; a permalink reaches the running client through the page's
`?open=` query. Accessibility is an ARIA mirror (`a11y_web.c`): the tree the
application publishes becomes a hidden layer of elements over the canvas,
each with its role, name and position, taking focus and activation from
assistive technology and the keyboard, an activation handed back as the same
invoke token the UIA route delivers; announcements go to a live region.

**Served by the daemon** (`daemon/webapp.h`). The daemon is the client's web
server, and a daemon always serves the client that matches it: the page and the
loader (`openchime.html`, `openchime.js`) are compiled into the binary as
resources (`scripts/build/embed_res.py`, the Makefile's `WEBAPP_RES`), so a
`WEB=1` daemon (`make full`) builds the web client first; the one thing outside
the binary is the wasm, shipped beside the executable as the voice data is
(`web/openchime.wasm` beside it, `/usr/share/openchime/web` when installed by a
package, which the .deb, .rpm, tarball and image all carry). On the TLS port
`/` sends a browser to `/app/`, which comes with `Cross-Origin-Opener-Policy:
same-origin` and `Cross-Origin-Embedder-Policy: require-corp` (wasm threads
need the isolation) and the loader and wasm under it. Nothing is configured:
without the wasm beside it the daemon says so once and the landing page stands.

**Sign-in in a browser.** The page starts the client as `openchime --signin
<host:port>`, its own origin, and nothing is typed into it: the client asks
the daemon the way in (`AUTH_BEGIN`, AUTH.md §8.1) naming its own origin at
`/app/` as the redirect -- which the daemon accepts in place of a loopback
address when it is the origin the page's WebSocket was upgraded from (the
`Host` of the upgrade, `oc_webapp_is_redirect`) -- and the page goes to the
daemon's sign-in page (§8.10) itself. The browser comes back to `/app/?token=`,
a new life of the page: the client takes the token off the address, takes back
the PKCE verifier and the source it stashed in the tab's session storage on the
way out (`client/core/signin_web.c`, the browser's `signin.c`), and starts the
core with the result (`oc_client_start_signin_result`), whose first connection
presents `AUTH{oidc}` as a desktop's does after its listener hears the
redirect. A stored session reconnects on a reload without visiting the page.
Under the test harness a demo build types for the person:
`?env=OPENCHIME_TEST_SIGNIN=user:password` has the page post the daemon's
sign-in form itself and follow the redirect back (`plat_web.c`).

**An invitation link.** `/app/?invite=<hex>` is the page opened with an
invitation (REQ-026): the page starts the client as `openchime --signin
<host:port> --invite <hex>`, the sign-in begins with the invitation, and the
daemon's page opens on the sign-up form, so a person who was sent the link
chooses a username and password in the browser and comes back here signed in
as the new member, with nothing installed. The clients offer the link in the
invitation text where the daemon's pages open directly
(`oc_client_pages_direct`); `scripts/web/invite.mjs` proves it from a cold
browser.

**Media in the browser.** The same media, voice and call code the desktop
links (`client/core/media`, `voice`, `call`: VP9 through libvpx, Opus,
speexdsp's canceller and preprocessor, the MP4 muxer, the recorder, the player,
the segmenter, the call engine with SFrame and its jitter buffers) is compiled
to wasm (`scripts/build/media_wasm.sh` builds the three libraries), over the
browser's two backends in place of Media Foundation, Graphics Capture and
miniaudio: the camera and a screen through `getUserMedia` and
`getDisplayMedia` (`cap_web.c`, VIDEO-MESSAGES.md §3.2) and the microphone
and speaker through Web Audio (`audio_web.c`, AUDIO.md §3.2). So a video
message is recorded, reviewed, sent and played back in the browser; voice
input segments speech and the daemon's recogniser answers; a call joins the
others' through the daemon's relay over the connection (`calls-tcp`, CALLS.md:
a page has no UDP, so the engine starts on the connection and never probes),
encrypted end to end as on a desktop; a screen is shared and a shared one
viewed. Verified in a headless Chromium with its fake camera and microphone,
two tabs in one call, the recogniser answering a spoken clip through the
synthetic microphone. The computer's own sound in a screen recording is the
audio the browser gives with the screen (`getDisplayMedia` with audio): a
tab's sound anywhere, the system's on Chromium for Windows and ChromeOS, and
none on Firefox or Safari, where the recording goes on without it as it does
on a machine that cannot give it. What a page cannot have here: UDP for a
call.

**Audio on the worklet.** A device is an AudioWorklet node (Emscripten's
worklet support, `-sAUDIO_WORKLET -sWASM_WORKERS`): its process callback is
this code, on the browser's audio rendering thread, calling the device
layer per 128-frame quantum exactly as miniaudio's callback does on a
desktop, so the far-end reference the echo canceller aligns against stays on
the media clock. One context per device rate; where the browser gave a
context another rate (Safari for a microphone), a speexdsp resampler runs in
the callback. A browser that cannot start the worklet falls back to a
`ScriptProcessorNode` on the page's thread (1024 frames of latency), and the
console says which ran (`oc-web: audio worklet`); `make web-test` requires
the worklet. The device lists come from a cache the page keeps
(`devices_web.c`), so any thread can ask; the labels appear once a device
has been allowed, as the browser decides.

**Text, the rest of the way.** The canvas backend lays text out itself, so
what DirectWrite does inside its layout the common core now does for it
(`sdltext/st_common.c`, over utf8proc, for any backend that lays out itself):
grapheme clusters (UAX #29), so an emoji sequence or a conjunct is never
split by a cut, a caret or a hit test; line break opportunities (a working
subset of UAX #14: after spaces and hyphens, around ideographs, never before
closing punctuation, inside a number or at a no-break space, never inside a
cluster); and bidirectional levels (UAX #9 without the bracket-pair rule),
with each right-to-left run measured and drawn with the canvas's direction
set and a line's runs placed in visual order, and alignment following the
paragraph's reading direction as DirectWrite's does. Tested on the host
(`tests/test_sdltext_unicode.c`) and in the browser through the client's
`sttest` verb, which `make web-test` runs.

**The input method.** A browser's IME composes into an element, so the
platform keeps a hidden `<input>` at the caret (`oc_plat_text_input_area`,
the contract's one line for it; Win32 answers with SDL's text input area)
and hands its composition to SDL's own events -- `SDL_EVENT_TEXT_EDITING`
while composing, `SDL_EVENT_TEXT_INPUT` at the commit -- so the composer
shows the composition underlined and takes the result as it does from a
desktop IME. `make web-test` plays a Japanese composition into it.

Rough edges: fonts are the browser's, so layouts are near-identical to
Windows, not pixel-identical, and the harness compares state, not pixels. the picker and the clipboard need a
user gesture, as the browser requires, so the harness drives uploads through
the tray and the test hook instead; there is no IME beyond what SDL's
Emscripten port passes through; a crash leaves nothing but the console.

## What is left, in order

1. **`make web-test` and `scripts/gui/run.sh --web all` in CI.** CI builds the client (the
   daemon needs it) but runs the browser tests locally only; a runner with a
   Chromium joins them.


What a page cannot have, and the application lives without: a tray, the
taskbar's flash and progress, the platform's certificate viewer, autostart,
the system's notification sounds, a crash dump, UDP for a call.

## Decisions taken here

- **No TLS inside the WebSocket.** The page's HTTPS secures the wire and the
  browser judges the certificate; a second TLS would cost a handshake and
  memory and protect nothing more. A deployment therefore serves `/ws` on
  443 (or behind the same front door), never in the clear beyond the box.
- **The protocol does not change.** Frames inside WebSocket messages are the
  frames of PROTOCOL.md §2; a WebSocket message may hold several or part of
  one. The daemon's send path wraps each send in one frame; the client reads
  payloads as a stream.
- **Threads, not a state machine.** The core's network thread runs as a
  pthread (Web Worker) under Emscripten with the socket proxied to the page,
  rather than rewriting `net.c` as callbacks. The cost is the two isolation
  headers on the page; the gain is the core compiled unchanged.
- **One application, a directory per platform.** The browser is a platform
  directory like Windows, not a port of the application: a Win32 shim over
  SDL3 was the first step that proved the application could run here, and
  the extraction ARCH-80 named replaced it with the contract every platform
  implements. Linux and macOS are the next two directories.

## Building it

```
make web                             # the client in build/web; the first run fetches Emscripten 6.0.8 (mk/toolchains.mk) and builds SDL3, mbedTLS and the codecs for wasm once
make WEB=1                           # the daemon with the client compiled in and web/openchime.wasm beside it (remembered; `make full` does the same)
make DEMO=1 web-test                 # gfx under Node, then the client in a headless Chromium (a demo build: the check signs in by password)
python3 scripts/web/serve.py build/web  # the dev server: http://127.0.0.1:8765/openchime.html?host=…&port=…&user=…&pass=…
```

A daemon started from this directory serves the client at `https://<its
address>:<TLS port>/` (self-signed in development, so the browser asks once).

The two wasm libraries are built once into `build/wasm/` and never touch the
native trees in `third_party/`. The page takes the workspace in its query
(`host`, `port`, `user`, `pass`, which become the application's command line;
without them the sign-in view opens) and, with `hook=1`, the client's test
hook on the page's filesystem (`ocHook('verb')`, `ocShot()`), which is what
the test drives. `make web-test` needs a Chromium: Playwright's download is
found under `~/.cache/ms-playwright`, or name one in `CHROME`.
