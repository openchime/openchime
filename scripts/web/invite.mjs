// An invitation link opened cold in a real browser (REQ-026; docs/TESTING.md,
// "The pages from a browser"): `https://<workspace>/app/?invite=<hex>` is the
// daemon's own web client started with the invitation. The client sends the
// browser to the daemon's sign-up form, the newcomer chooses a username and
// password, and the browser comes back to the web client signed in as them,
// with nothing installed. scripts/web/pages.sh runs it after pages.mjs.
//
//   node scripts/web/invite.mjs <origin> <invite-hex> [chromium]
//
// One line per check on stdout; exit 1 when any fails.
import { chromium } from 'playwright-core';

const [origin, invite, exe] = process.argv.slice(2);
if (!origin || !invite) { console.error('usage: invite.mjs <origin> <invite-hex> [chromium]'); process.exit(2); }

const browser = await chromium.launch({ headless: true, executablePath: exe || undefined, args: ['--ignore-certificate-errors'] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
const page = await ctx.newPage();
const say = (s) => console.log(s);
let failures = 0;
const check = (ok, what) => { if (ok) say('  ok   ' + what); else { say('  FAIL ' + what); failures++; } };
const until = async (what, pred, ms) => {
  const t0 = Date.now();
  while (!(await pred())) {
    if (Date.now() - t0 > ms) throw new Error('timed out waiting for ' + what + '; the page is at ' + page.url());
    await page.waitForTimeout(100);
  }
};

// 1. The link: the web client starts with the invitation and sends the
// browser to the sign-up form. ?hook=1 keeps the client's test hook for the
// tab, which is how its state is read afterwards (openchime.html).
await page.goto(`${origin}/app/?invite=${invite}&hook=1`);
await until('the sign-up form', async () => (await page.locator('form[action="signup"]').count()) === 1, 60000);
check(page.url().startsWith(origin + '/signin?'), 'the link opens the daemon\'s sign-up form');
check(new URL(page.url()).searchParams.get('invite') === invite, 'with the invitation');

// 2. The newcomer chooses a name and a password and is sent back to the web client.
await page.fill('input[name="username"]', 'linked');
await page.fill('input[name="password"]', 'pw-linked');
await page.fill('input[name="confirm"]', 'pw-linked');
await page.click('button[type="submit"]');
await until('the web client', async () => new URL(page.url()).pathname === '/app/', 30000);
check(!page.url().includes('invite='), 'the browser comes back to the web client');

// 3. The client takes the token off the address and signs in: its own dump
// says so. The hook answers once the wasm is up, so it is asked until it does.
let dump = '';
await until('the client to sign in', async () => {
  dump = await page.evaluate(async () => {
    try {
      if (typeof ocHook !== 'function' || typeof FS === 'undefined') return '';
      await ocHook('dump /t/d.txt', 5000);
      return FS.readFile('/t/d.txt', { encoding: 'utf8' });
    } catch (e) { return ''; }
  });
  return /authed=1 connected=1/.test(dump);
}, 60000).catch((e) => { say('  FAIL ' + e.message); failures++; });
check(/authed=1 connected=1/.test(dump), 'the new member is signed in to the web client');
check(!page.url().includes('token='), 'the token is taken off the address');
await browser.close();
process.exit(failures ? 1 : 0);
