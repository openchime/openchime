// The web client's load (docs/WEB.md, "Loading"), from a real browser: the
// load screen counts the download in bytes, names each stage after it and
// comes down when the client has drawn; the wasm and the loader arrive
// gzipped; and a second load in the same browser downloads next to nothing,
// every route revalidated to 304. scripts/web/pages.sh runs it.
//
//   node scripts/web/load.mjs <origin> <spki-sha256-base64> [chromium]
//
// The daemon's certificate is trusted by its key (the SPKI hash), not ignored:
// a browser keeps nothing in its HTTP cache from a certificate it was told to
// overlook, and the second load would prove nothing. The page dials a port
// nothing listens on, so the client draws its first frame and stays (a
// workspace that answered would send it to the sign-in page).
// One line per check on stdout; the exit status is the verdict.
import { chromium } from 'playwright-core';

const [origin, spki, exe] = process.argv.slice(2);
if (!origin || !spki) { console.error('usage: load.mjs <origin> <spki-sha256-base64> [chromium]'); process.exit(2); }

const browser = await chromium.launch({
  headless: true, executablePath: exe || undefined,
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader',
         '--ignore-certificate-errors-spki-list=' + spki],
});
const ctx = await browser.newContext();
// Every state the load screen passes through, kept by the page as it happens:
// on the same host a download is over in a few frames, too quick to poll.
await ctx.addInitScript(() => {
  window.__ocStages = [];
  document.addEventListener('DOMContentLoaded', () => {
    const l = document.getElementById('load');
    if (!l) return;
    const st = l.querySelector('.stage'), tr = l.querySelector('.track');
    const rec = () => {
      const s = { text: st.textContent, now: tr.getAttribute('aria-valuenow'), failed: l.classList.contains('failed') };
      const last = window.__ocStages[window.__ocStages.length - 1];
      if (!last || last.text !== s.text || last.now !== s.now) window.__ocStages.push(s);
    };
    new MutationObserver(rec).observe(l, { subtree: true, childList: true, characterData: true, attributes: true });
    rec();
  });
});
const page = await ctx.newPage();
const cdp = await ctx.newCDPSession(page);
await cdp.send('Network.enable');
let failures = 0;
const check = (ok, what) => { console.log((ok ? '  ok   ' : '  FAIL ') + what); if (!ok) failures++; };
const url = origin + '/app/?host=127.0.0.1&port=1';

// One load: what came over the wire per file, and the load screen's stages.
async function load() {
  const name = {}, wire = {}, enc = {};
  const onResp = (e) => {
    const f = new URL(e.response.url).pathname.split('/').pop() || 'page';
    name[e.requestId] = f;
    enc[f] = e.response.headers['content-encoding'] || e.response.headers['Content-Encoding'] || '';
  };
  const onDone = (e) => { const f = name[e.requestId]; if (f) wire[f] = (wire[f] || 0) + e.encodedDataLength; };
  cdp.on('Network.responseReceived', onResp);
  cdp.on('Network.loadingFinished', onDone);
  await page.goto(url);
  let gone = false, failed = '';
  for (let i = 0; i < 1200 && !gone && !failed; i++) {
    const s = await page.evaluate(() => {
      const l = document.getElementById('load');
      return l ? { failed: l.classList.contains('failed'), text: l.querySelector('.stage').textContent } : { gone: true };
    }).catch(() => null);
    if (s && s.gone) gone = true;
    else if (s && s.failed) failed = s.text;
    else await page.waitForTimeout(100);
  }
  const stages = await page.evaluate(() => window.__ocStages || []).catch(() => []);
  await page.waitForTimeout(300);   // the last files' loadingFinished
  cdp.off('Network.responseReceived', onResp);
  cdp.off('Network.loadingFinished', onDone);
  return { stages, gone, failed, wire, enc };
}

const cold = await load();
check(!cold.failed, 'the load screen did not fail' + (cold.failed ? ': ' + cold.failed : ''));
check(cold.gone, 'the load screen came down when the client drew itself');
const counted = cold.stages.find((s) => /^Downloading [0-9.]+ of [0-9.]+ MB$/.test(s.text) && s.now !== null);
check(!!counted, 'the download was counted in bytes against its size, with the bar\'s value (' + (counted ? counted.text : 'never') + ')');
const order = ['Downloading', 'Starting', 'Opening'].map((w) => cold.stages.findIndex((s) => s.text.startsWith(w)));
check(order.every((i, k) => i >= 0 && (k === 0 || i > order[k - 1])), 'Downloading, Starting and Opening were shown in that order');
check(cold.enc['openchime.wasm'] === 'gzip' && cold.enc['openchime.js'] === 'gzip', 'the wasm and the loader arrived gzipped');
const wasmKB = (cold.wire['openchime.wasm'] || 0) / 1024;
check(wasmKB > 100 && wasmKB < 4096, `the wasm came over the wire once, ${wasmKB.toFixed(0)} KB`);

const warm = await load();
check(warm.gone && !warm.failed, 'a second load came up too');
const again = ['page', 'openchime.js', 'openchime.wasm'].map((f) => [f, warm.wire[f] || 0]);
check(again.every(([, b]) => b < 2048), 'a second load downloaded nothing again: ' +
      again.map(([f, b]) => `${f} ${(b / 1024).toFixed(1)} KB`).join(', '));

await browser.close();
process.exit(failures ? 1 : 0);
