// Camera-return thesis test for the world-space GI cache (docs/WORLDGI.md §7.4).
// The cache's claimed advantage is persistence: a surface that leaves the frame
// and returns has no screen-space temporal history, but still lives in its
// brick/face world cell. Protocol on interior_static (pose B, converged ref):
//   1. warm N frames at B (cache learns B's GI; screen history converges),
//   2. look away to A (yaw+pi) for N frames (accumulation forgets B; A seeds
//      different cells, so B's cells persist),
//   3. return to B and capture at +[2,4,8,16] frames, scoring HDR-FLIP vs the
//      frozen interior_static reference.
// worldgi=0 is the exact screen-only control. A lower post-return FLIP for
// worldgi=1 at small offsets is the persistence win.
import { chromium } from 'playwright';
import http from 'node:http';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromiumLaunchOptions } from './gpu-launch.mjs';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const MIME = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wgsl': 'text/plain', '.json': 'application/json', '.css': 'text/css' };
const W = 1920, H = 1080;
const REF = join(ROOT, 'test/eval/refs/interior_static_1920x1080_s1_rb12_rf1600.rgbf32');
const TMP = join(ROOT, 'test/eval/runs');
const poseB = { px: 8.8, py: 4.7, pz: 7.0, yaw: -0.45, pitch: 0.02 };
const poseA = { ...poseB, yaw: poseB.yaw + Math.PI };   // look the opposite way
const WARM = 60, AWAY = 60, OFFSETS = [2, 4, 8, 16];
const python = process.env.PYTHON || join(process.env.USERPROFILE || '', '.cache',
  'codex-runtimes', 'codex-primary-runtime', 'dependencies', 'python', 'python.exe');

const server = http.createServer(async (req, res) => {
  try {
    let p = req.url.split('?')[0];
    if (p === '/') p = '/index.html';
    const file = normalize(join(ROOT, p));
    if (!file.startsWith(normalize(ROOT))) throw new Error('forbidden');
    res.writeHead(200, { 'content-type': MIME[extname(file)] || 'application/octet-stream' });
    res.end(await readFile(file));
  } catch { res.writeHead(404); res.end('not found'); }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const port = server.address().port;
const browser = await chromium.launch(chromiumLaunchOptions({ benchmark: true }));
const page = await browser.newPage({ viewport: { width: W, height: H }, deviceScaleFactor: 1 });
page.on('console', (m) => { if (m.type() === 'error') console.log('[page:error]', m.text()); });
await mkdir(TMP, { recursive: true });

const frames = () => page.evaluate(() => window.__voxelrt.frames);
const waitTo = (n) => page.waitForFunction(
  (t) => window.__voxelrt && (window.__voxelrt.error || window.__voxelrt.frames >= t),
  n, { timeout: 120000, polling: 50 });
const setPose = (p) => page.evaluate((q) => window.__voxelrt.setPose(q), p);

async function score(rawBuf) {
  const testPath = join(TMP, `return_tmp.rgbf32`);
  await writeFile(testPath, rawBuf);
  const r = spawnSync(python, [join(ROOT, 'test/metrics.py'),
    '--ref', REF, '--test', testPath, '--width', String(W), '--height', String(H), '--flip'],
    { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`metrics.py failed: ${r.stderr || r.stdout}`);
  return JSON.parse(r.stdout);
}

async function returnRun(worldgi) {
  const q = `preset=ours&worldgi=${worldgi}&fclamp=0&bounces=6&scale=1&nocanvas=1` +
    `&px=${poseB.px}&py=${poseB.py}&pz=${poseB.pz}&yaw=${poseB.yaw}&pitch=${poseB.pitch}`;
  await page.goto(`http://127.0.0.1:${port}/?${q}`);
  await waitTo(WARM);                                   // warm at B
  await setPose(poseA);
  await waitTo((await frames()) + AWAY);                // look away at A
  await setPose(poseB);                                 // return to B
  const fR = await frames();
  const out = {};
  for (const off of OFFSETS) {
    await waitTo(fR + off);
    const cap = await page.evaluate(() => window.__voxelrt.capture({ hdr: true }));
    out[off] = await score(Buffer.from(cap.hdr, 'base64'));
  }
  return out;
}

try {
  if (!existsSync(REF)) throw new Error(`missing reference ${REF}`);
  console.log(`camera-return: warm ${WARM} @B, away ${AWAY} @A(yaw+pi), return @B; FLIP vs interior_static ref`);
  const off = await returnRun(0);
  const on = await returnRun(1);
  console.log(`\n offset |  ours(ctrl) FLIP | worldgi FLIP | delta`);
  for (const k of OFFSETS) {
    const a = off[k].flip, b = on[k].flip;
    const d = a != null && b != null ? `${((b - a) / a * 100).toFixed(1)}%` : 'n/a';
    console.log(`  +${String(k).padStart(2)}   |   ${a?.toFixed(4)}        |   ${b?.toFixed(4)}     | ${d}`);
  }
  console.log('\n(relRMSE / PSNR for context)');
  for (const k of OFFSETS) {
    console.log(`  +${k}: ctrl relRmse ${off[k].hdrRelativeRmse.toFixed(4)} psnr ${off[k].hdrPsnrPeak.toFixed(1)} | ` +
      `worldgi relRmse ${on[k].hdrRelativeRmse.toFixed(4)} psnr ${on[k].hdrPsnrPeak.toFixed(1)}`);
  }
} finally {
  await browser.close();
  server.close();
}
