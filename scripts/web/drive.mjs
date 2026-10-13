// The web client's driver, the browser half (docs/TESTING.md, "Driving the
// web client"). scripts/web/drive.sh starts one of these per client; it holds
// a headless Chromium on the daemon's own /app/ page and relays the harness's
// command channel to the client's test hook, so a feature script written for
// the Windows driver runs unchanged against the browser:
//
//   <dir>/cmd  -> the verb, written by the shell side (cmd.tmp then renamed)
//   <dir>/ack  -> the client's answer, written here once the hook has acked
//   dump <name>, shot <name> -> <dir>/<name>.txt, <dir>/<name>.bmp, taken out
//                               of the page's filesystem as the Windows client
//                               writes them to its own scratch directory
//   attach <path> -> the file copied onto the page's filesystem first, so the
//                    verb names a path the client can read
//   <dir>/quit -> the page and the browser are closed and this process ends
//
//   node scripts/web/drive.mjs <dir> <url> [chromium-executable]
//
// The development inputs a Windows script exports (OPENCHIME_DEV_AUDIO, _MIC,
// _MIC_ECHO, _CAPTURE, _TONE, _VIDEO_CAP_MS, _SCREEN_GONE_MS) reach the page
// as ?env=; a _MIC WAV is routed to the page as /app/mic.wav. The browser
// runs with Chromium's fake camera and microphone and answers its own
// permission prompts, as the Windows client runs with the synthetic devices.
import fs from 'node:fs';
import path from 'node:path';
import { chromium } from 'playwright-core';

const [dir, baseUrl, exe] = process.argv.slice(2);
if (!dir || !baseUrl) { console.error('usage: scripts/web/drive.mjs <dir> <url> [chromium]'); process.exit(2); }
fs.mkdirSync(dir, { recursive: true });
const log = (s) => fs.appendFileSync(path.join(dir, 'drive.log'), `${new Date().toISOString()} ${s}\n`);

const url = new URL(baseUrl);
const pass = ['OPENCHIME_DEV_AUDIO', 'OPENCHIME_DEV_MIC_ECHO', 'OPENCHIME_DEV_CAPTURE', 'OPENCHIME_DEV_TONE',
              'OPENCHIME_DEV_VIDEO_CAP_MS', 'OPENCHIME_DEV_SCREEN_GONE_MS', 'OPENCHIME_DEV_MIC_LEAD_MS'];
for (const k of pass) if (process.env[k]) url.searchParams.append('env', `${k}=${process.env[k]}`);
const mic = process.env.OPENCHIME_DEV_MIC;
if (mic) { url.searchParams.append('env', 'OPENCHIME_DEV_MIC=/mic.wav'); url.searchParams.set('mic', 'mic.wav'); }
url.searchParams.set('hook', '1');

// OC_TEST_DEBUG_PORT=<n>: Chromium's remote debugging on that port (a `b`
// client takes the next), for a debugger to attach to a stuck page.
const dbg = process.env.OC_TEST_DEBUG_PORT ? [`--remote-debugging-port=${parseInt(process.env.OC_TEST_DEBUG_PORT, 10) + (dir.endsWith('-b') ? 1 : 0)}`] : [];
// A profile kept per client name under the cache directory: the compiled
// wasm stays cached across runs, which is most of a fresh page's sign-in time.
const profile = path.join(process.env.XDG_CACHE_HOME || path.join(process.env.HOME || '/tmp', '.cache'), 'openchime-webdrive',
                          path.resolve(dir).replace(/^\/+/, '').replace(/[^A-Za-z0-9_.-]+/g, '-'));
fs.mkdirSync(profile, { recursive: true });
// A browser that died leaves its lock behind; nothing else holds this profile.
for (const f of ['SingletonLock', 'SingletonSocket', 'SingletonCookie']) { try { fs.unlinkSync(path.join(profile, f)); } catch (e) {} }
const ctx = await chromium.launchPersistentContext(profile, {
  headless: true,
  executablePath: exe || undefined,
  ignoreHTTPSErrors: true, viewport: { width: 1280, height: 800 },
  permissions: ['camera', 'microphone', 'clipboard-read', 'clipboard-write', 'notifications'],
  args: [...dbg, '--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader', '--ignore-certificate-errors',
         '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
         '--autoplay-policy=no-user-gesture-required', '--auto-select-desktop-capture-source=Entire screen'],
});
// A stored session from an earlier run must not sign the page in as someone else.
try { await ctx.clearCookies(); } catch (e) {}
if (mic) await ctx.route((u) => u.pathname.endsWith('/mic.wav'), (route) => route.fulfill({ path: mic, contentType: 'audio/wav' }));
const page = ctx.pages()[0] || await ctx.newPage();
// Once, on the first load: the sign-in leaves for the daemon's page and comes
// back, and what the page keeps for the tab (its hook, the development inputs) stays.
await page.addInitScript(() => { try { if (!sessionStorage.getItem('oc.drive')) { localStorage.clear(); sessionStorage.clear(); sessionStorage.setItem('oc.drive', '1'); } } catch (e) {} });
page.on('console', (m) => log('console: ' + m.text().slice(0, 300)));
page.on('pageerror', (e) => log('pageerror: ' + e.message));
await page.goto(url.toString());
log('opened ' + url.toString());

// The page's filesystem, from here.
// A file onto the page, in pieces: a large upload (the harness tries 120 MB)
// as one base64 string would not cross the bridge.
async function fsWrite(p, bytes) {
  const CHUNK = 4 << 20;
  for (let at = 0; at < bytes.length || at === 0; at += CHUNK) {
    const piece = Buffer.from(bytes.subarray(at, Math.min(at + CHUNK, bytes.length))).toString('base64');
    await page.evaluate(([p, b64, first]) => {
      if (first) { const d = p.slice(0, p.lastIndexOf('/')); try { FS.mkdirTree(d); } catch (e) {} }
      const bin = atob(b64), out = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
      const f = FS.open(p, first ? 'w' : 'a'); FS.write(f, out, 0, out.length); FS.close(f);
    }, [p, piece, at === 0]);
    if (bytes.length === 0) break;
  }
}
// Text as text; bytes as base64 (a screenshot is megabytes, which as a
// number array would never finish crossing the bridge).
const fsRead = (p, enc) => page.evaluate(([p, enc]) => {
  try {
    const d = FS.readFile(p, enc ? { encoding: enc } : {});
    if (enc) return d;
    let s = ''; for (let i = 0; i < d.length; i += 0x8000) s += String.fromCharCode.apply(null, d.subarray(i, i + 0x8000));
    return btoa(s);
  } catch (e) { return null; }
}, [p, enc]);
const fsUnlink = (p) => page.evaluate((p) => { try { FS.unlink(p); } catch (e) {} }, p);

// The hook's channel on the page: the client reads /t\cmd and writes /t\ack
// (its directory joined with a backslash, as on Windows), on its 30 ms tick.
const HOOK = '/t\\';
async function hook(cmd, timeoutMs) {
  await fsUnlink(HOOK + 'ack');
  await page.evaluate((c) => { FS.writeFile('/t\\cmd', c); }, cmd);
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    const a = await fsRead(HOOK + 'ack', 'utf8');
    if (a !== null) return a;
    await new Promise((r) => setTimeout(r, 20));
  }
  return 'TIMEOUT';
}

// The shell side's channel: the same files, in <dir>.
const cmdFile = path.join(dir, 'cmd'), ackFile = path.join(dir, 'ack'), quitFile = path.join(dir, 'quit');
const writeAck = (text) => { fs.writeFileSync(ackFile + '.tmp', text); fs.renameSync(ackFile + '.tmp', ackFile); };

async function serve(line) {
  const sp = line.indexOf(' ');
  const verb = sp < 0 ? line : line.slice(0, sp), arg = sp < 0 ? '' : line.slice(sp + 1);
  let cmd = line, after = null;
  if ((verb === 'dump' || verb === 'shot' || verb === 'shotfull') && arg && !arg.includes('/') && !arg.includes('\\')) {
    // A bare name: the file comes out of the page as <dir>/<name>.<ext>.
    const ext = verb === 'dump' ? 'txt' : 'bmp', onPage = `/out/${arg}.${ext}`;
    cmd = `${verb} ${onPage}`;
    await page.evaluate(() => { try { FS.mkdir('/out'); } catch (e) {} });
    after = async () => {
      const b64 = await fsRead(onPage, null);
      if (b64) fs.writeFileSync(path.join(dir, `${arg}.${ext}`), Buffer.from(b64, 'base64'));
    };
  } else if ((verb === 'attach' || verb === 'upload') && arg && fs.existsSync(arg)) {
    // A file of this machine's, placed where the client can read it.
    const onPage = '/up/' + path.basename(arg);
    await fsWrite(onPage, fs.readFileSync(arg));
    cmd = `${verb} ${onPage}`;
  }
  const wait = verb === 'shot' || verb === 'shotfull' ? 15000 : 10000;
  const ack = await hook(cmd, wait);
  if (after && ack !== 'TIMEOUT') await after();
  writeAck(ack);
}

log('serving');
for (;;) {
  if (fs.existsSync(quitFile)) break;
  if (fs.existsSync(cmdFile)) {
    let line = '';
    try { line = fs.readFileSync(cmdFile, 'utf8'); fs.unlinkSync(cmdFile); } catch (e) { continue; }
    try { await serve(line); } catch (e) { log('serve error: ' + e.message); writeAck('TIMEOUT'); }
    continue;
  }
  await new Promise((r) => setTimeout(r, 20));
}
try { fs.unlinkSync(quitFile); } catch (e) {}
await ctx.close();
log('closed');
