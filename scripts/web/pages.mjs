// The daemon's sign-in pages, driven by a real browser (docs/TESTING.md, "The
// pages from a browser"): a headless Chromium types into the forms as a person
// does and submits them, so what the browser sends -- its Origin under the
// pages' referrer policy, its handling of the 303 -- is what is proven, not
// what a scripted client sends. scripts/web/pages.sh runs it.
//
//   node scripts/web/pages.mjs <origin> <setup-token-hex> <challenge> <callback-url> [chromium]
//
// Each sign-in's redirect is `callback-url`, a loopback listener the script
// runs (a browser follows the 303 at its network layer, past anything a test
// can intercept), and the token is read off the address the browser lands on.
// One line per check on stdout; last, `token <jwt>` from the final sign-in,
// for the script to present on AUTH with the challenge's verifier.
import { chromium } from 'playwright-core';

const [origin, setup, challenge, callback, exe] = process.argv.slice(2);
if (!origin || !setup || !challenge || !callback) { console.error('usage: pages.mjs <origin> <setup-token> <challenge> <callback-url> [chromium]'); process.exit(2); }

const browser = await chromium.launch({ headless: true, executablePath: exe || undefined, args: ['--ignore-certificate-errors'] });
const ctx = await browser.newContext({ ignoreHTTPSErrors: true });
const page = await ctx.newPage();
const redirect = encodeURIComponent(callback);
const cbPath = new URL(callback).host.replace(/\./g, '\\.');
const say = (s) => console.log(s);
let failures = 0;
const check = (ok, what) => { if (ok) say('  ok   ' + what); else { say('  FAIL ' + what); failures++; } };
const tokenOf = () => { try { return new URL(page.url()).searchParams.get('token') || ''; } catch (e) { return ''; } };
const msg = async () => (await page.locator('p.msg').first().textContent().catch(() => '')) || '';
// Submit, then wait until the address is the callback's: polled, since the
// browser may already be there by the time the click resolves.
const submitToCallback = async () => {
  await page.click('button[type="submit"]');
  const re = new RegExp(cbPath), t0 = Date.now();
  while (!re.test(page.url())) {
    if (Date.now() - t0 > 30000) throw new Error('no callback within 30 s; the page is at ' + page.url() + ': ' + (await msg()));
    await page.waitForTimeout(100);
  }
};
const submitStays = async () => { await page.click('button[type="submit"]'); await page.waitForSelector('p.msg', { timeout: 30000 }); };

// 1. Sign up with the setup token: the first owner, made on the page.
await page.goto(`${origin}/signin?redirect_uri=${redirect}&nonce=${challenge}&invite=${setup}`);
check(await page.locator('form[action="signup"]').count() === 1, 'the invitation opens the sign-up form');
check(await page.locator('input[name="invite"][type="hidden"]').count() === 1, 'the link carries the invitation out of sight');
check((await page.textContent('form')).includes('At least 15 characters'), 'the rule is said beside the field');
await page.fill('input[name="username"]', 'owen');
await page.fill('input[name="password"]', 'pw-owen');
await page.fill('input[name="confirm"]', 'pw-owen');
await submitStays();
check((await msg()).includes('at least 15 characters'), 'a short password is refused, saying why');
await page.fill('input[name="password"]', 'lantern-orchard-copper');
await page.fill('input[name="confirm"]', 'lantern-orchard-copper');
check(await page.isVisible('#showpw'), 'the show-password toggle is offered');
await page.check('#showpw');
check(await page.getAttribute('input[name="password"]', 'type') === 'text', 'ticked, it shows the password');
await page.uncheck('#showpw');
await submitToCallback();
check(tokenOf().length > 20, 'sign-up ends at the callback with a token');

// 2. The sign-in form: a wrong password is refused on the page, the right one signs in.
await page.goto(`${origin}/signin?redirect_uri=${redirect}&nonce=${challenge}`);
await page.fill('input[name="username"]', 'owen');
await page.fill('input[name="password"]', 'not-it');
await submitStays();
check((await msg()).includes("isn't right"), 'a wrong password shows the form again, saying so');
await page.fill('input[name="password"]', 'lantern-orchard-copper');
await submitToCallback();
check(tokenOf().length > 20, 'the right password ends at the callback with a token');

// 3. The password page: the current password checked, the new one stored.
await page.goto(`${origin}/account/password`);
await page.fill('input[name="username"]', 'owen');
await page.fill('input[name="current"]', 'lantern-orchard-copper');
await page.fill('input[name="password"]', 'granite-harbor-violet');
await page.fill('input[name="confirm"]', 'granite-harbor-violet');
await page.click('button[type="submit"]');
await page.waitForSelector('h1', { timeout: 30000 });
check((await page.locator('h1').first().textContent()) === 'Password changed', 'the password page changes it');

// 4. The old password is spent and the new one signs in; that token is the script's to present.
await page.goto(`${origin}/signin?redirect_uri=${redirect}&nonce=${challenge}`);
await page.fill('input[name="username"]', 'owen');
await page.fill('input[name="password"]', 'lantern-orchard-copper');
await submitStays();
check((await msg()).includes("isn't right"), 'the old password is refused');
await page.fill('input[name="password"]', 'granite-harbor-violet');
await submitToCallback();
const token = tokenOf();
check(token.length > 20, 'the new password signs in');
say('token ' + token);
await browser.close();
process.exit(failures ? 1 : 0);
