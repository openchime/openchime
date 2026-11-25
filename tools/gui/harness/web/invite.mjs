// An invitation link opened cold in a real browser (REQ-026; tools/gui/README.md,
// "web_pages"): `https://<workspace>/app/?invite=<hex>` is the
// daemon's own web client started with the invitation. The client sends the
// browser to the daemon's sign-up form, the newcomer chooses a username and
// password, and the browser comes back to the web client signed in as them,
// with nothing installed. tools/gui/features/web_pages.sh runs it after pages.mjs.
//
//   node tools/gui/harness/web/invite.mjs <origin> <invite-hex> [chromium]
//
// One line per check on stdout; exit 1 when any fails.
import { chromium } from 'playwright-core';

const [origin, invite, exe] = process.argv.slice(2);
if (!origin || !invite) { console.error('usage: invite.mjs <origin> <invite-hex> [chromium]'); process.exit(2); }

const browser = await chromium.launch({ headless: true, executablePath: exe || undefined, args: ['--ignore-certificate-errors'] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
let page = await ctx.newPage();
const say = (s) => console.log(s);
let failures = 0;
const check = (ok, what) => { if (ok) say('  ok   ' + what); else { say('  FAIL ' + what); failures++; } };
// Playwright waits on each condition itself; a wait that runs out says what it
// was waiting for and where the page is.
const within = async (what, wait) => {
  try { return await wait; }
  catch (e) { throw new Error('timed out waiting for ' + what + '; the page is at ' + page.url()); }
};
// The client's own dump, once it says the client is signed in. The hook answers
// once the wasm is up and takes its time, so the page asks it one dump at a
// time and keeps the newest; the wait looks at that until it says so.
const signedIn = (what) => within(what, page.waitForFunction(() => {
  const s = window.__ocSignIn || (window.__ocSignIn = { busy: false, dump: '' });
  if (/authed=1 connected=1/.test(s.dump)) return s.dump;
  if (!s.busy && typeof ocHook === 'function' && typeof FS !== 'undefined') {
    s.busy = true;
    Promise.resolve().then(() => ocHook('dump /t/d.txt', 5000))
      .then(() => { s.dump = FS.readFile('/t/d.txt', { encoding: 'utf8' }); })
      .catch(() => {})
      .finally(() => { s.busy = false; });
  }
  return false;
}, null, { timeout: 60000, polling: 100 }).then((h) => h.jsonValue()));

// 1. The link: the page sends the browser to the sign-up form before the web
// client loads -- the client is needed only once the account exists. ?hook=1
// keeps the client's test hook for the tab, which is how its state is read
// afterwards (openchime.html).
let wasmAt = 0;
page.on('request', (req) => { if (!wasmAt && req.url().endsWith('/openchime.wasm')) wasmAt = Date.now(); });
await page.goto(`${origin}/app/?invite=${invite}&hook=1`);
await within('the sign-up form', page.waitForSelector('form[action="signup"]', { state: 'attached', timeout: 60000 }));
const formAt = Date.now();
check(page.url().startsWith(origin + '/signin?'), 'the link opens the daemon\'s sign-up form');
check(!wasmAt || wasmAt >= formAt - 1000, 'without loading the web client first');
check(new URL(page.url()).searchParams.get('invite') === invite, 'with the invitation');

// 2. The newcomer chooses a name and a password and is sent back to the web client.
await page.fill('input[name="username"]', 'linked');
await page.fill('input[name="password"]', 'pebble-canyon-orbit');
await page.fill('input[name="confirm"]', 'pebble-canyon-orbit');
await page.click('button[type="submit"]');
await within('the web client', page.waitForURL((u) => u.pathname === '/app/', { waitUntil: 'commit', timeout: 30000 }));
check(!page.url().includes('invite='), 'the browser comes back to the web client');

// 3. The client takes the token off the address and signs in: its own dump
// says so.
const dump = await signedIn('the client to sign in').catch((e) => { say('  FAIL ' + e.message); failures++; return ''; });
check(/authed=1 connected=1/.test(dump), 'the new member is signed in to the web client');
check(!page.url().includes('token='), 'the token is taken off the address');

// 4. The tab closed and the web client opened again: the session is kept, as
// any remembered one is, so the newcomer is the same person, still signed in.
await page.close();
page = await ctx.newPage();
await page.goto(`${origin}/app/?hook=1`);
const dump2 = await signedIn('the client to sign in again').catch((e) => { say('  FAIL ' + e.message); failures++; return ''; });
check(/authed=1 connected=1/.test(dump2), 'a closed tab keeps the session');
await browser.close();
process.exit(failures ? 1 : 0);
