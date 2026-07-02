// Headless smoke test: boots the renderer in Chromium with WebGPU, waits for
// frames to be produced, then captures a screenshot and verifies the image is
// not blank.
//
// Usage: node test/screenshot.mjs [--frames N] [--out path.png]

import { chromium } from 'playwright';
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  assessAdapter,
  chromiumLaunchOptions,
  detectNvidiaGpu,
  getArg,
  hasFlag,
} from './gpu-launch.mjs';

const args = process.argv.slice(2);
const arg = (name, dflt) => getArg(args, name, dflt);
const flag = (name) => hasFlag(args, name);
const WAIT_FRAMES = parseInt(arg('--frames', '4'), 10);
const OUT = arg('--out', 'test/screenshot.png');
const ROOT = fileURLToPath(new URL('..', import.meta.url));
const allowSoftwareGpu = flag('--allow-software-gpu');
const allowNonNvidiaGpu = flag('--allow-non-nvidia-gpu');
const explicitRequireNvidia = flag('--require-nvidia-gpu') || process.env.VOXELRT_REQUIRE_NVIDIA_GPU === '1';
const nvidiaGpus = detectNvidiaGpu();

const MIME = {
  '.html': 'text/html', '.js': 'text/javascript', '.wgsl': 'text/plain', '.png': 'image/png',
};

const server = http.createServer(async (req, res) => {
  try {
    let path = req.url.split('?')[0];
    if (path === '/') path = '/index.html';
    const file = normalize(join(ROOT, path));
    if (!file.startsWith(normalize(ROOT))) throw new Error('forbidden');
    const data = await readFile(file);
    res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(data);
  } catch {
    console.log('[404]', req.url);
    res.writeHead(404); res.end('not found');
  }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const launch = chromiumLaunchOptions({ benchmark: false });
const browser = await chromium.launch(launch);

const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
page.on('console', (m) => console.log(`[page:${m.type()}]`, m.text()));
page.on('pageerror', (e) => console.log('[pageerror]', e.message));

// A short 1920x1080 run keeps the smoke test quick without changing the
// resolution used by the browser benchmark path.
const query = arg('--query', 'scale=1&bounces=1&nocanvas=1');
await page.goto(`http://127.0.0.1:${port}/?${query}`);

try {
  await page.waitForFunction(
    (n) => window.__voxelrt && (window.__voxelrt.error || window.__voxelrt.frames >= n),
    WAIT_FRAMES,
    { timeout: 300000, polling: 500 }
  );
} catch (e) {
  const st = await page.evaluate(() => window.__voxelrt);
  console.error('TIMEOUT waiting for frames. state =', JSON.stringify(st));
  await page.screenshot({ path: OUT });
  await browser.close();
  server.close();
  process.exit(1);
}

const state = await page.evaluate(() => window.__voxelrt);
if (state.error) {
  console.error('RENDERER ERROR:', state.error);
  await browser.close();
  server.close();
  process.exit(1);
}
const requireNvidia = explicitRequireNvidia || Boolean(nvidiaGpus?.length && !allowNonNvidiaGpu);
const assessed = assessAdapter(state.adapterInfo, {
  requireHardware: !allowSoftwareGpu,
  requireNvidia,
  label: 'Chromium WebGPU adapter',
});
if (nvidiaGpus?.length) console.log(`Host NVIDIA GPU(s): ${nvidiaGpus.join('; ')}`);
console.log(`Chromium WebGPU adapter: ${assessed.text}`);
for (const warning of assessed.warnings) console.warn(`WARNING: ${warning}`);

// Read the rendered image straight off the GPU (canvas presentation is not
// composited in some headless environments, so a page screenshot can be
// blank even when the renderer works).
const cap = await page.evaluate(() => window.__voxelrt.capture());
const png = Buffer.from(cap.png.split(',')[1], 'base64');
await import('node:fs/promises').then((fs) => fs.writeFile(OUT, png));

console.log(`frames=${state.frames} size=${cap.w}x${cap.h} mean=${cap.mean.toFixed(1)} std=${cap.std.toFixed(1)}`);
await browser.close();
server.close();

if (cap.mean < 2 && cap.std < 2) {
  console.error('FAIL: rendered image appears blank');
  process.exit(1);
}
console.log(`OK — render captured to ${OUT}`);
