// Standalone acceptance check for the world-space GI cache (docs/WORLDGI.md §7):
//   1. Inert when empty: worldgi=1 vs worldgi=0 at frame 1 must be byte-identical
//      HDR (empty cache => every query merge is the identity).
//   2. Fills and contributes: at 32 frames the two must differ (cache active).
// Diagnostic only; mirrors bench.mjs's server + launch + capture path.
import { chromium } from 'playwright';
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromiumLaunchOptions } from './gpu-launch.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wgsl': 'text/plain', '.json': 'application/json', '.css': 'text/css' };

const server = http.createServer(async (req, res) => {
  try {
    let path = req.url.split('?')[0];
    if (path === '/') path = '/index.html';
    const file = normalize(join(ROOT, path));
    if (!file.startsWith(normalize(ROOT))) throw new Error('forbidden');
    const data = await readFile(file);
    res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(data);
  } catch { res.writeHead(404); res.end('not found'); }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const browser = await chromium.launch(chromiumLaunchOptions({ benchmark: true }));
const page = await browser.newPage({ viewport: { width: 640, height: 360 }, deviceScaleFactor: 1 });
page.on('console', (m) => { if (m.type() === 'error') console.log('[page:error]', m.text()); });

async function renderHdr(query, frames) {
  const url = `http://127.0.0.1:${port}/?${query}&stopat=${frames}&nocanvas=1`;
  await page.goto(url);
  await page.waitForFunction(
    (n) => window.__voxelrt && (window.__voxelrt.error || window.__voxelrt.frames >= n),
    frames, { timeout: 120000, polling: 100 });
  const st = await page.evaluate(() => ({ error: window.__voxelrt.error }));
  if (st.error) throw new Error(`renderer: ${st.error} (${query})`);
  const cap = await page.evaluate(() => window.__voxelrt.capture({ hdr: true }));
  return Buffer.from(cap.hdr, 'base64');
}

function compare(a, b) {
  if (a.length !== b.length) return { identical: false, note: `length ${a.length} vs ${b.length}` };
  const fa = new Float32Array(a.buffer, a.byteOffset, a.length / 4);
  const fb = new Float32Array(b.buffer, b.byteOffset, b.length / 4);
  let diffBytes = 0, maxAbs = 0, sumA = 0, sumB = 0, n = fa.length;
  for (let i = 0; i < n; i++) {
    if (fa[i] !== fb[i]) diffBytes++;
    maxAbs = Math.max(maxAbs, Math.abs(fa[i] - fb[i]));
    sumA += fa[i]; sumB += fb[i];
  }
  return { identical: diffBytes === 0, diffChannels: diffBytes, total: n,
    maxAbs, meanA: sumA / n, meanB: sumB / n };
}

const base = 'preset=ours';
try {
  console.log('== Test 0: determinism control (worldgi=0 twice, frame 1) ==');
  const ctlA = await renderHdr(`${base}&worldgi=0`, 1);
  const ctlB = await renderHdr(`${base}&worldgi=0`, 1);
  console.log(JSON.stringify(compare(ctlA, ctlB)));

  console.log('== Test 1: inert when empty (frame 1) ==');
  const off1 = await renderHdr(`${base}&worldgi=0`, 1);
  const on1 = await renderHdr(`${base}&worldgi=1`, 1);
  const r1 = compare(off1, on1);
  console.log(JSON.stringify(r1));
  console.log(r1.identical ? 'PASS: byte-identical (cache inert when empty)'
                           : 'FAIL: differs while cache should be empty');

  console.log('== Test 2: fills and contributes (frame 32) ==');
  const off32 = await renderHdr(`${base}&worldgi=0`, 32);
  const on32 = await renderHdr(`${base}&worldgi=1`, 32);
  const r32 = compare(off32, on32);
  console.log(JSON.stringify(r32));
  const relMeanDelta = Math.abs(r32.meanB - r32.meanA) / Math.max(r32.meanA, 1e-6);
  console.log(`rel mean delta ${(relMeanDelta * 100).toFixed(3)}% | changed channels ${(100 * r32.diffChannels / r32.total).toFixed(1)}%`);
  console.log(!r32.identical ? 'PASS: cache active (image changed)' : 'FAIL: no effect at 32 frames');
} finally {
  await browser.close();
  server.close();
}
