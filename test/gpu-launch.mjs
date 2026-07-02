import { existsSync, readdirSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';

export function hasFlag(args, name) {
  return args.includes(name);
}

export function getArg(args, name, dflt) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : dflt;
}

export function findChromiumExecutable() {
  if (process.env.CHROMIUM_PATH) return process.env.CHROMIUM_PATH;
  if (process.platform === 'win32' && process.env.LOCALAPPDATA) {
    const root = join(process.env.LOCALAPPDATA, 'ms-playwright');
    if (existsSync(root)) {
      const dirs = readdirSync(root)
        .filter((d) => /^chromium-\d+$/.test(d))
        .sort()
        .reverse();
      for (const d of dirs) {
        const exe = join(root, d, 'chrome-win64', 'chrome.exe');
        if (existsSync(exe)) return exe;
      }
    }
  }
  return null;
}

export function chromiumGpuArgs({ benchmark = false } = {}) {
  const args = [
    '--no-sandbox',
    '--enable-unsafe-webgpu',
    '--force_high_performance_gpu',
    '--ignore-gpu-blocklist',
    '--disable-software-rasterizer',
  ];
  if (benchmark) {
    args.push(
      '--disable-gpu-vsync',
      '--disable-frame-rate-limit',
      '--disable-renderer-backgrounding',
      '--disable-background-timer-throttling',
      '--disable-backgrounding-occluded-windows',
    );
  }
  return args;
}

export function chromiumLaunchOptions({ benchmark = false } = {}) {
  const launch = { args: chromiumGpuArgs({ benchmark }) };
  const executablePath = findChromiumExecutable();
  if (executablePath) launch.executablePath = executablePath;
  return launch;
}

function adapterText(adapterInfo) {
  if (!adapterInfo) return 'unknown adapter';
  return [
    adapterInfo.vendor,
    adapterInfo.architecture,
    adapterInfo.device,
    adapterInfo.description,
  ].filter(Boolean).join(' / ') || JSON.stringify(adapterInfo);
}

export function assessAdapter(adapterInfo, {
  requireNvidia = false,
  requireHardware = true,
  label = 'WebGPU adapter',
} = {}) {
  const text = adapterText(adapterInfo);
  const lower = text.toLowerCase();
  const warnings = [];
  const software = /\b(swiftshader|llvmpipe|software|warp|microsoft basic render)\b/i.test(text) ||
    adapterInfo?.isFallbackAdapter === true;
  const nvidia = /\b(nvidia|geforce|rtx|gtx|quadro|0x10de)\b/i.test(text);
  const integrated = /\b(intel|uhd|iris|integrated|radeon graphics|0x8086)\b/i.test(text) && !nvidia;

  if (software) warnings.push(`${label} appears to be software/fallback: ${text}`);
  if (integrated) warnings.push(`${label} appears to be integrated graphics: ${text}`);
  if (requireHardware && software) throw new Error(warnings[0]);
  if (requireNvidia && !nvidia) throw new Error(`${label} is not NVIDIA/RTX: ${text}`);
  return { text, software, integrated, nvidia, warnings };
}

export function detectNvidiaGpu() {
  if (process.platform !== 'win32') return null;
  const r = spawnSync('nvidia-smi', ['--query-gpu=name', '--format=csv,noheader'], {
    encoding: 'utf8',
    windowsHide: true,
  });
  if (r.status !== 0) return null;
  const names = r.stdout.split(/\r?\n/).map((s) => s.trim()).filter(Boolean);
  return names.length ? names : null;
}
