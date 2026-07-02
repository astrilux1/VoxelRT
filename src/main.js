import { Camera } from './camera.js';
import { generateScene, VOXEL_SIZE, BRICK } from './scene.js';
import { normalize3 } from './math.js';
import { makePairingBuffer } from './pairing.js';

// ---------------------------------------------------------------------------
// Real-time path-traced global illumination for a 1/16 m voxel world,
// sampled through a unified ReSTIR reservoir pipeline (after Lin et al. 2026,
// "ReSTIR PT Enhanced"). Passes per frame (all WGSL compute except the blit):
//   1. pathtrace      — primary hit + initial reservoir candidates (sun NEE,
//                       emissive light-list NEE, BSDF bounce path)
//   2. reuse_temporal — reprojected reservoir merge, duplication-adaptive cCap
//   3. reuse_spatial  — paired Gaussian reuse + vector-weight shading
//   4. dupmap         — sample duplication map for next frame's cCap
//   5. temporal       — camera-reprojected exponential history accumulation
//   6. atrous ×3      — edge-aware wavelet denoise, relaxing as history grows
//   7. present        — remodulate albedo, expose, ACES tonemap
// Every ReSTIR feature is flag-gated for benchmarking (?preset=..., below).
// ---------------------------------------------------------------------------

const params = new URLSearchParams(location.search);
const GRID = parseInt(params.get('grid') || '256', 10);
const SCENE_VARIANT = params.get('scene') || 'default';
let renderScale = parseFloat(params.get('scale') || '1');
let bounces = parseInt(params.get('bounces') || '2', 10);
let exposure = 1.0;
let denoiseOn = params.get('denoise') !== '0';
let temporalOn = params.get('temporal') !== '0';
let sunAnimate = false;
let sunAzimuth = 0.9;
let sunElevation = 0.85;

// --- ReSTIR configuration (flag bits mirror common.wgsl) --------------------
const RF = {
  restir: 1, treuse: 2, sreuse: 4, paired: 8, dupmap: 16, footprint: 32,
  vector: 64, unified: 128, plane: 256, rescue: 512, fullv: 1024, rclamp: 2048,
  lightpower: 4096,
};
const PRESETS = {
  // Plain 1-spp path tracer + temporal accumulation + à-trous (pre-ReSTIR).
  base: [],
  // ReSTIR GI-style: reservoirs for indirect only, random-disk spatial reuse.
  gi: ['restir', 'treuse', 'sreuse'],
  // Faithful adaptation of the Lin 2026 technique set.
  lin: ['restir', 'treuse', 'sreuse', 'paired', 'dupmap', 'footprint',
        'vector', 'unified'],
  // Ours: Lin 2026 + voxel-exact plane reuse, disocclusion rescue,
  // per-candidate visibility, reservoir contribution clamp, light power sampling.
  ours: ['restir', 'treuse', 'sreuse', 'paired', 'dupmap', 'footprint',
         'vector', 'unified', 'plane', 'rescue', 'fullv', 'rclamp', 'lightpower'],
};
const preset = params.get('preset') || 'ours';
let restirFlags = (PRESETS[preset] || PRESETS.ours)
  .reduce((m, k) => m | RF[k], 0);
for (const k of Object.keys(RF)) {          // per-flag URL overrides
  const v = params.get(k);
  if (v === '1') restirFlags |= RF[k];
  if (v === '0') restirFlags &= ~RF[k];
}
const tuning = {
  taps: parseInt(params.get('taps') || '3', 10),
  sigma: parseFloat(params.get('sigma') || '16'),      // pairing texture σ (px)
  radius: parseFloat(params.get('radius') || '30'),    // random-disk radius (px)
  ccap: parseFloat(params.get('ccap') || '20'),
  capmin: parseFloat(params.get('capmin') || '1'),
  dupalpha: parseFloat(params.get('dupalpha') || '0.1'),
  fpc: parseFloat(params.get('fpc') || '0.0008'),
  clamp: parseFloat(params.get('rclampv') || '24'),
  maxhist: parseFloat(params.get('maxhist') || '64'),
  fseed: parseInt(params.get('fseed') || '0', 10),
};
// Deterministic camera strafe for benchmarking temporal behavior, and an
// exact frame count to halt at so captures land on a reproducible pose.
const benchMove = parseFloat(params.get('benchmove') || '0');
const stopAt = parseInt(params.get('stopat') || '0', 10);

const status = { frames: 0, ready: false, error: null };
window.__voxelrt = status;

const hud = document.getElementById('hud');
const canvas = document.getElementById('gfx');

function fatal(msg) {
  status.error = String(msg);
  document.getElementById('overlay').textContent = String(msg);
  document.getElementById('overlay').style.display = 'flex';
  console.error(msg);
}

async function loadShader(name) {
  const r = await fetch(`src/shaders/${name}`);
  if (!r.ok) throw new Error(`failed to load ${name}`);
  return r.text();
}

async function makeModule(device, label, code) {
  const module = device.createShaderModule({ label, code });
  const info = await module.getCompilationInfo();
  const errs = info.messages.filter((m) => m.type === 'error');
  if (errs.length) {
    throw new Error(`${label} compile errors:\n` +
      errs.map((m) => `${m.lineNum}:${m.linePos} ${m.message}`).join('\n'));
  }
  return module;
}

function adapterLimits(limits) {
  return {
    maxTextureDimension2D: limits.maxTextureDimension2D,
    maxStorageBufferBindingSize: limits.maxStorageBufferBindingSize,
    maxStorageBuffersPerShaderStage: limits.maxStorageBuffersPerShaderStage,
    maxComputeInvocationsPerWorkgroup: limits.maxComputeInvocationsPerWorkgroup,
    maxComputeWorkgroupSizeX: limits.maxComputeWorkgroupSizeX,
    maxComputeWorkgroupSizeY: limits.maxComputeWorkgroupSizeY,
    maxComputeWorkgroupSizeZ: limits.maxComputeWorkgroupSizeZ,
    maxComputeWorkgroupsPerDimension: limits.maxComputeWorkgroupsPerDimension,
  };
}

async function init() {
  if (!navigator.gpu) throw new Error('WebGPU is not available in this browser.');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('No WebGPU adapter found.');
  const wantTiming = params.get('timing') === '1';
  const timingSupported = wantTiming && adapter.features?.has?.('timestamp-query');
  const device = await adapter.requestDevice({
    requiredFeatures: timingSupported ? ['timestamp-query'] : [],
  });
  device.addEventListener('uncapturederror', (e) => fatal(e.error.message));
  device.lost.then((info) => {
    if (info.reason !== 'destroyed') fatal(`WebGPU device lost: ${info.message}`);
  });
  status.adapterInfo = {
    vendor: adapter.info?.vendor, architecture: adapter.info?.architecture,
    device: adapter.info?.device, description: adapter.info?.description,
    isFallbackAdapter: adapter.isFallbackAdapter,
    features: Array.from(adapter.features || []),
    limits: adapterLimits(adapter.limits),
  };
  console.log('adapter:', JSON.stringify(status.adapterInfo));
  status.gpuTiming = {
    requested: wantTiming,
    supported: timingSupported,
    frames: 0,
    sums: {},
    latest: {},
  };

  // Headless WebGPU builds can lose the device when a canvas surface is
  // configured; ?nocanvas=1 renders offscreen only (read via capture()).
  const noCanvas = params.get('nocanvas') === '1';
  const canvasFormat = navigator.gpu.getPreferredCanvasFormat();
  let ctx = null;
  if (!noCanvas) {
    ctx = canvas.getContext('webgpu');
    ctx.configure({ device, format: canvasFormat, alphaMode: 'opaque' });
  }

  // --- Scene ---------------------------------------------------------------
  const scene = generateScene(GRID, SCENE_VARIANT);
  const voxelBuf = device.createBuffer({
    label: 'voxels', size: scene.vox.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  device.queue.writeBuffer(voxelBuf, 0, scene.vox);
  const brickBuf = device.createBuffer({
    label: 'bricks', size: scene.bricks.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  device.queue.writeBuffer(brickBuf, 0, scene.bricks);
  const brickMaskBuf = device.createBuffer({
    label: 'brickMasks', size: scene.brickMasks.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  device.queue.writeBuffer(brickMaskBuf, 0, scene.brickMasks);

  const uniformBuf = device.createBuffer({
    label: 'uniforms', size: 272,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });

  // Emissive-face light list (unified DI+GI candidates).
  const lightBuf = device.createBuffer({
    label: 'lights', size: scene.lights.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  device.queue.writeBuffer(lightBuf, 0, scene.lights);
  const lightAliasBuf = device.createBuffer({
    label: 'lightAlias', size: scene.lightAlias.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  device.queue.writeBuffer(lightAliasBuf, 0, scene.lightAlias);

  // Self-inverting Gaussian pairing textures for paired spatial reuse.
  const pairingData = makePairingBuffer(Math.max(0.8, tuning.sigma));
  const pairingBuf = device.createBuffer({
    label: 'pairing', size: pairingData.byteLength,
    usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST,
  });
  device.queue.writeBuffer(pairingBuf, 0, pairingData);

  // Per-iteration step size for the à-trous passes.
  const atrousParamBufs = [1, 2, 4].map((step) => {
    const b = device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(b, 0, new Int32Array([step, 0, 0, 0]));
    return b;
  });

  // --- Shaders -------------------------------------------------------------
  const constants =
    `const GRID : i32 = ${GRID};\nconst BRICK : i32 = ${BRICK};\nconst VOXEL_SIZE : f32 = ${VOXEL_SIZE};\n`;
  const common = constants + (await loadShader('common.wgsl'));
  const restirLib = await loadShader('restir.wgsl');
  const voxelLib = await loadShader('voxel.wgsl');
  const [ptModule, trModule, spModule, dupModule, tModule, aModule, prModule] =
    await Promise.all([
      makeModule(device, 'pathtrace', common + restirLib + voxelLib + (await loadShader('pathtrace.wgsl'))),
      makeModule(device, 'reuse_temporal', common + restirLib + (await loadShader('reuse_temporal.wgsl'))),
      makeModule(device, 'reuse_spatial', common + restirLib + voxelLib + (await loadShader('reuse_spatial.wgsl'))),
      makeModule(device, 'dupmap', common + restirLib + (await loadShader('dupmap.wgsl'))),
      makeModule(device, 'temporal', common + (await loadShader('temporal.wgsl'))),
      makeModule(device, 'atrous', common + (await loadShader('atrous.wgsl'))),
      makeModule(device, 'present', await loadShader('present.wgsl')),
    ]);

  const ptPipeline = device.createComputePipeline({
    label: 'pathtrace', layout: 'auto', compute: { module: ptModule, entryPoint: 'main' },
  });
  const trPipeline = device.createComputePipeline({
    label: 'reuse_temporal', layout: 'auto', compute: { module: trModule, entryPoint: 'main' },
  });
  const spPipeline = device.createComputePipeline({
    label: 'reuse_spatial', layout: 'auto', compute: { module: spModule, entryPoint: 'main' },
  });
  const dupPipeline = device.createComputePipeline({
    label: 'dupmap', layout: 'auto', compute: { module: dupModule, entryPoint: 'main' },
  });
  const tPipeline = device.createComputePipeline({
    label: 'temporal', layout: 'auto', compute: { module: tModule, entryPoint: 'main' },
  });
  const aPipeline = device.createComputePipeline({
    label: 'atrous', layout: 'auto', compute: { module: aModule, entryPoint: 'main' },
  });
  const prPipeline = device.createRenderPipeline({
    label: 'present', layout: 'auto',
    vertex: { module: prModule, entryPoint: 'vsMain' },
    fragment: { module: prModule, entryPoint: 'fsMain', targets: [{ format: canvasFormat }] },
    primitive: { topology: 'triangle-list' },
  });

  const linearSampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });

  // --- Size-dependent resources -------------------------------------------
  let tex = null;
  let reservoirBufA = null, reservoirBufB = null;
  let rw = 0, rh = 0;
  const textureForView = new WeakMap();

  function makeTex(label, format) {
    const texture = device.createTexture({
      label, format, size: [rw, rh],
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC,
    });
    const view = texture.createView();
    textureForView.set(view, texture);
    return view;
  }

  function recreateTargets() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.max(8, Math.floor(canvas.clientWidth * dpr));
    canvas.height = Math.max(8, Math.floor(canvas.clientHeight * dpr));
    rw = Math.max(8, Math.floor(canvas.width * renderScale));
    rh = Math.max(8, Math.floor(canvas.height * renderScale));
    tex = {
      radiance: makeTex('radiance', 'rgba16float'),
      direct: makeTex('direct', 'rgba16float'),
      dup: makeTex('dup', 'r32float'),
      albedo: makeTex('albedo', 'rgba8unorm'),
      gbuf: [makeTex('gbufA', 'rgba32float'), makeTex('gbufB', 'rgba32float')],
      accum: [makeTex('accumA', 'rgba16float'), makeTex('accumB', 'rgba16float')],
      denoise: [makeTex('denoiseA', 'rgba16float'), makeTex('denoiseB', 'rgba16float')],
    };
    // Reservoirs: 32 bytes per pixel; A = working set (initial + temporal),
    // B = post-spatial output, read back as next frame's temporal history.
    // WebGPU zero-initializes buffers, so a fresh B decodes as "no sample".
    for (const b of [reservoirBufA, reservoirBufB]) if (b) b.destroy();
    reservoirBufA = device.createBuffer({
      label: 'reservoirA', size: rw * rh * 32, usage: GPUBufferUsage.STORAGE,
    });
    reservoirBufB = device.createBuffer({
      label: 'reservoirB', size: rw * rh * 32, usage: GPUBufferUsage.STORAGE,
    });
  }
  recreateTargets();
  let resizePending = false;
  window.addEventListener('resize', () => { resizePending = true; });

  // --- Camera + uniforms ---------------------------------------------------
  // Optional camera pose overrides (useful for scripted captures).
  const spawn = { ...scene.spawn };
  if (params.has('px')) spawn.pos = [parseFloat(params.get('px')), parseFloat(params.get('py')), parseFloat(params.get('pz'))];
  if (params.has('yaw')) spawn.yaw = parseFloat(params.get('yaw'));
  if (params.has('pitch')) spawn.pitch = parseFloat(params.get('pitch'));
  const camera = new Camera(canvas, spawn);
  const uniformData = new ArrayBuffer(272);
  const f32 = new Float32Array(uniformData);
  const u32 = new Uint32Array(uniformData);
  let prevViewProj = null;
  let prevCamPos = [...camera.pos];
  let frame = 0;
  const timingWarmup = parseInt(params.get('timewarmup') || '4', 10);
  const maxTimingQueries = 32;
  const timingQuerySet = timingSupported
    ? device.createQuerySet({ type: 'timestamp', count: maxTimingQueries })
    : null;
  const timingResolveBuf = timingSupported
    ? device.createBuffer({
        size: maxTimingQueries * 8,
        usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC,
      })
    : null;

  function writeUniforms(time) {
    const { viewProj, invViewProj } = camera.matrices(rw / rh);
    if (!prevViewProj) prevViewProj = viewProj;
    const sunDir = normalize3([
      Math.cos(sunElevation) * Math.cos(sunAzimuth),
      Math.sin(sunElevation),
      Math.cos(sunElevation) * Math.sin(sunAzimuth),
    ]);
    f32.set(invViewProj, 0);
    f32.set(prevViewProj, 16);
    f32.set([...camera.pos, time], 32);
    f32.set([...prevCamPos, 0], 36);
    f32.set([...sunDir, Math.cos(0.03)], 40);          // ~1.7° angular radius
    f32.set([5.0, 4.5, 3.8, 1.0], 44);                 // sun irradiance, sky intensity
    u32.set([frame + tuning.fseed, bounces, temporalOn ? 1 : 0, 0], 48);
    f32.set([rw, rh, exposure, 0], 52);
    u32.set([restirFlags, scene.lightCount, tuning.taps, 0], 56);
    f32.set([tuning.ccap, tuning.capmin, tuning.dupalpha, tuning.fpc], 60);
    f32.set([tuning.radius, tuning.maxhist, tuning.clamp, 0], 64);
    device.queue.writeBuffer(uniformBuf, 0, uniformData);
    prevViewProj = viewProj;
    prevCamPos = [...camera.pos];
  }

  // --- Bind groups (recreated each frame: cheap, and ping-pong friendly) ---
  function frameBindGroups(parity) {
    const gCur = tex.gbuf[parity], gPrev = tex.gbuf[1 - parity];
    const aCur = tex.accum[parity], aPrev = tex.accum[1 - parity];

    const pt = device.createBindGroup({
      layout: ptPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: uniformBuf } },
        { binding: 1, resource: { buffer: voxelBuf } },
        { binding: 2, resource: { buffer: brickBuf } },
        { binding: 3, resource: tex.radiance },
        { binding: 4, resource: tex.albedo },
        { binding: 5, resource: gCur },
        { binding: 6, resource: { buffer: lightBuf } },
        { binding: 7, resource: { buffer: reservoirBufA } },
        { binding: 8, resource: tex.direct },
        { binding: 9, resource: { buffer: lightAliasBuf } },
        { binding: 10, resource: { buffer: brickMaskBuf } },
      ],
    });
    const tr = device.createBindGroup({
      layout: trPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: uniformBuf } },
        { binding: 4, resource: gCur },
        { binding: 6, resource: gPrev },
        { binding: 7, resource: { buffer: reservoirBufA } },
        { binding: 9, resource: { buffer: reservoirBufB } },
        { binding: 12, resource: tex.dup },
      ],
    });
    const sp = device.createBindGroup({
      layout: spPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: uniformBuf } },
        { binding: 1, resource: { buffer: voxelBuf } },
        { binding: 2, resource: { buffer: brickBuf } },
        { binding: 3, resource: tex.radiance },
        { binding: 4, resource: gCur },
        { binding: 7, resource: { buffer: reservoirBufA } },
        { binding: 8, resource: tex.direct },
        { binding: 9, resource: { buffer: reservoirBufB } },
        { binding: 10, resource: { buffer: brickMaskBuf } },
        { binding: 13, resource: { buffer: pairingBuf } },
      ],
    });
    const dup = device.createBindGroup({
      layout: dupPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: uniformBuf } },
        { binding: 9, resource: { buffer: reservoirBufB } },
        { binding: 14, resource: tex.dup },
      ],
    });
    const tp = device.createBindGroup({
      layout: tPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: uniformBuf } },
        { binding: 3, resource: tex.radiance },
        { binding: 4, resource: gCur },
        { binding: 5, resource: aPrev },
        { binding: 6, resource: gPrev },
        { binding: 7, resource: aCur },
        { binding: 8, resource: linearSampler },
      ],
    });
    // à-trous ping-pong: accum -> dA -> dB -> dA
    const chain = [
      [aCur, tex.denoise[0], atrousParamBufs[0]],
      [tex.denoise[0], tex.denoise[1], atrousParamBufs[1]],
      [tex.denoise[1], tex.denoise[0], atrousParamBufs[2]],
    ];
    const at = chain.map(([src, dst, pbuf]) => device.createBindGroup({
      layout: aPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: uniformBuf } },
        { binding: 3, resource: src },
        { binding: 4, resource: gCur },
        { binding: 5, resource: dst },
        { binding: 6, resource: { buffer: pbuf } },
      ],
    }));
    const finalTex = denoiseOn ? tex.denoise[0] : aCur;
    const pr = device.createBindGroup({
      layout: prPipeline.getBindGroupLayout(0),
      entries: [
        { binding: 0, resource: { buffer: uniformBuf } },
        { binding: 1, resource: finalTex },
        { binding: 2, resource: tex.albedo },
        { binding: 3, resource: linearSampler },
      ],
    });
    return { pt, tr, sp, dup, tp, at, pr };
  }

  // --- Offscreen capture (used by the headless test; canvas presentation is
  // not composited in some headless environments, so this reads the rendered
  // image straight off the GPU) ---------------------------------------------
  let lastParity = 0;
  function bytesToBase64(bytes) {
    let bin = '';
    for (let i = 0; i < bytes.length; i += 8192) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));
    }
    return btoa(bin);
  }

  function halfToFloat(h) {
    const s = (h & 0x8000) ? -1 : 1;
    const e = (h >> 10) & 0x1f;
    const f = h & 0x03ff;
    if (e === 0) return s * Math.pow(2, -14) * (f / 1024);
    if (e === 31) return f ? NaN : s * Infinity;
    return s * Math.pow(2, e - 15) * (1 + f / 1024);
  }

  async function readTextureBytes(view, bytesPerPixel) {
    const texture = textureForView.get(view);
    if (!texture) throw new Error('capture texture is missing');
    const bpr = Math.ceil((rw * bytesPerPixel) / 256) * 256;
    const buf = device.createBuffer({
      size: bpr * rh,
      usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
    });
    const enc = device.createCommandEncoder();
    enc.copyTextureToBuffer({ texture }, { buffer: buf, bytesPerRow: bpr }, [rw, rh]);
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const bytes = new Uint8Array(buf.getMappedRange()).slice();
    buf.unmap();
    buf.destroy();
    return { bytes, bpr };
  }

  status.capture = async (opts = {}) => {
    const target = device.createTexture({
      format: canvasFormat, size: [rw, rh],
      usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC,
    });
    const bpr = Math.ceil((rw * 4) / 256) * 256;
    const buf = device.createBuffer({ size: bpr * rh, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
    const enc = device.createCommandEncoder();
    const rp = enc.beginRenderPass({
      colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store' }],
    });
    rp.setPipeline(prPipeline);
    rp.setBindGroup(0, frameBindGroups(lastParity).pr);
    rp.draw(3);
    rp.end();
    enc.copyTextureToBuffer({ texture: target }, { buffer: buf, bytesPerRow: bpr }, [rw, rh]);
    device.queue.submit([enc.finish()]);
    await buf.mapAsync(GPUMapMode.READ);
    const raw = new Uint8Array(buf.getMappedRange());
    const bgra = canvasFormat.startsWith('bgra');

    const c2 = document.createElement('canvas');
    c2.width = rw; c2.height = rh;
    const g2 = c2.getContext('2d');
    const img = g2.createImageData(rw, rh);
    let sum = 0, sum2 = 0, n = 0;
    for (let y = 0; y < rh; y++) {
      for (let x = 0; x < rw; x++) {
        const si = y * bpr + x * 4, di = (y * rw + x) * 4;
        const r = bgra ? raw[si + 2] : raw[si];
        const b = bgra ? raw[si] : raw[si + 2];
        img.data[di] = r; img.data[di + 1] = raw[si + 1]; img.data[di + 2] = b; img.data[di + 3] = 255;
        const lum = (r + raw[si + 1] + b) / 3;
        sum += lum; sum2 += lum * lum; n++;
      }
    }
    g2.putImageData(img, 0, 0);
    buf.unmap(); buf.destroy(); target.destroy();
    const mean = sum / n;
    // Tightly packed RGB bytes as base64, for exact off-page comparison.
    let rgbB64 = '';
    {
      const rgb = new Uint8Array(rw * rh * 3);
      for (let i = 0, j = 0; i < rw * rh; i++) {
        rgb[j++] = img.data[i * 4];
        rgb[j++] = img.data[i * 4 + 1];
        rgb[j++] = img.data[i * 4 + 2];
      }
      rgbB64 = bytesToBase64(rgb);
    }

    const out = {
      png: c2.toDataURL('image/png'),
      rgb: rgbB64,
      mean, std: Math.sqrt(Math.max(0, sum2 / n - mean * mean)),
      w: rw, h: rh,
    };
    if (opts.hdr) {
      const finalView = denoiseOn ? tex.denoise[0] : tex.accum[lastParity];
      const [{ bytes: illum, bpr: illumBpr }, { bytes: alb, bpr: albBpr }] = await Promise.all([
        readTextureBytes(finalView, 8),
        readTextureBytes(tex.albedo, 4),
      ]);
      const hdr = new Float32Array(rw * rh * 3);
      for (let y = 0, j = 0; y < rh; y++) {
        for (let x = 0; x < rw; x++) {
          const hi = y * illumBpr + x * 8;
          const ai = y * albBpr + x * 4;
          const ir = halfToFloat(illum[hi] | (illum[hi + 1] << 8));
          const ig = halfToFloat(illum[hi + 2] | (illum[hi + 3] << 8));
          const ib = halfToFloat(illum[hi + 4] | (illum[hi + 5] << 8));
          const ar = alb[ai] / 255;
          const ag = alb[ai + 1] / 255;
          const ab = alb[ai + 2] / 255;
          hdr[j++] = ir * ar * ar;
          hdr[j++] = ig * ag * ag;
          hdr[j++] = ib * ab * ab;
        }
      }
      out.hdr = bytesToBase64(new Uint8Array(hdr.buffer));
      out.hdrFormat = 'rgb-f32le-linear';
    }
    return out;
  };

  // --- Hotkeys -------------------------------------------------------------
  window.addEventListener('keydown', (e) => {
    if (e.code.startsWith('Digit')) {
      const n = parseInt(e.code.slice(5), 10);
      if (n >= 1 && n <= 6) bounces = n;
    }
    if (e.code === 'KeyF') denoiseOn = !denoiseOn;
    if (e.code === 'KeyT') temporalOn = !temporalOn;
    if (e.code === 'KeyL') sunAnimate = !sunAnimate;
    if (e.code === 'BracketLeft') { renderScale = Math.max(0.25, renderScale - 0.25); resizePending = true; }
    if (e.code === 'BracketRight') { renderScale = Math.min(2, renderScale + 0.25); resizePending = true; }
  });

  // --- Frame loop ----------------------------------------------------------
  let lastTime = performance.now();
  let fpsAccum = 0, fpsCount = 0, fps = 0;

  function tick(nowMs) {
    try {
      const dt = Math.min(0.1, (nowMs - lastTime) / 1000);
      lastTime = nowMs;
      if (resizePending) { recreateTargets(); resizePending = false; }

      camera.update(dt);
      if (benchMove) {
        // Deterministic strafe (frame-indexed, not wall-clock) so benchmark
        // runs of temporal behavior are reproducible.
        camera.pos[0] = spawn.pos[0] + Math.sin(frame * 0.07) * benchMove;
      }
      if (sunAnimate) sunAzimuth += dt * 0.15;
      writeUniforms(nowMs / 1000);

      const parity = frame & 1;
      // Surface validation errors explicitly for the first few frames
      // (uncapturederror delivery is unreliable on some implementations).
      const checkErrors = frame < 3;
      if (checkErrors) {
        device.pushErrorScope('validation');
        device.pushErrorScope('out-of-memory');
      }
      const bg = frameBindGroups(parity);
      const enc = device.createCommandEncoder();
      const wg = [Math.ceil(rw / 8), Math.ceil(rh / 8)];
      const timingPasses = [];
      let timingQuery = 0;
      let timingReadBuf = null;
      const frameForTiming = frame;

      const passDesc = (label) => {
        if (!timingSupported || timingQuery + 1 >= maxTimingQueries) {
          return { label };
        }
        const begin = timingQuery++;
        const end = timingQuery++;
        timingPasses.push({ label, begin, end });
        return {
          label,
          timestampWrites: {
            querySet: timingQuerySet,
            beginningOfPassWriteIndex: begin,
            endOfPassWriteIndex: end,
          },
        };
      };

      const compute = (label, pipeline, group) => {
        const pass = enc.beginComputePass(passDesc(label));
        pass.setPipeline(pipeline);
        pass.setBindGroup(0, group);
        pass.dispatchWorkgroups(wg[0], wg[1]);
        pass.end();
      };

      compute('pathtrace', ptPipeline, bg.pt);
      if (restirFlags & RF.restir) {
        if (restirFlags & RF.treuse) compute('reuse_temporal', trPipeline, bg.tr);
        compute('reuse_spatial', spPipeline, bg.sp);
        if (restirFlags & RF.dupmap) compute('dupmap', dupPipeline, bg.dup);
      }
      compute('temporal', tPipeline, bg.tp);

      if (denoiseOn) {
        for (const group of bg.at) compute('atrous', aPipeline, group);
      }

      if (ctx) {
        const rp = enc.beginRenderPass({
          ...passDesc('present'),
          colorAttachments: [{
            view: ctx.getCurrentTexture().createView(),
            loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 },
          }],
        });
        rp.setPipeline(prPipeline);
        rp.setBindGroup(0, bg.pr);
        rp.draw(3);
        rp.end();
      }

      if (timingSupported && timingQuery > 0) {
        timingReadBuf = device.createBuffer({
          size: timingQuery * 8,
          usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ,
        });
        enc.resolveQuerySet(timingQuerySet, 0, timingQuery, timingResolveBuf, 0);
        enc.copyBufferToBuffer(timingResolveBuf, 0, timingReadBuf, 0, timingQuery * 8);
      }

      device.queue.submit([enc.finish()]);
      if (timingReadBuf) {
        timingReadBuf.mapAsync(GPUMapMode.READ).then(() => {
          const raw = new BigUint64Array(timingReadBuf.getMappedRange());
          const latest = {};
          for (const p of timingPasses) {
            const deltaNs = raw[p.end] > raw[p.begin] ? raw[p.end] - raw[p.begin] : 0n;
            latest[p.label] = Number(deltaNs) / 1e6;
          }
          status.gpuTiming.latest = latest;
          if (frameForTiming >= timingWarmup) {
            for (const [label, ms] of Object.entries(latest)) {
              status.gpuTiming.sums[label] = (status.gpuTiming.sums[label] || 0) + ms;
            }
            status.gpuTiming.frames++;
          }
          timingReadBuf.unmap();
          timingReadBuf.destroy();
        }).catch((e) => {
          status.gpuTiming.error = String(e);
          timingReadBuf.destroy();
        });
      }
      if (checkErrors) {
        Promise.all([device.popErrorScope(), device.popErrorScope()]).then(([oom, val]) => {
          if (oom) fatal(`out-of-memory: ${oom.message}`);
          if (val) fatal(`validation: ${val.message}`);
        }).catch(() => { /* device lost; reported via device.lost */ });
      }

      lastParity = parity;
      frame++;
      status.frames = frame;
      status.frameMs = (status.frameMs || dt * 1000) * 0.9 + dt * 1000 * 0.1;
      status.ready = true;

      fpsAccum += dt; fpsCount++;
      if (fpsAccum >= 0.5) {
        fps = fpsCount / fpsAccum;
        fpsAccum = 0; fpsCount = 0;
        hud.textContent =
          `${fps.toFixed(0)} fps | ${rw}x${rh} (scale ${renderScale.toFixed(2)}) | ` +
          `grid ${GRID}³ @ ${(VOXEL_SIZE * 100).toFixed(2)} cm voxels | ` +
          `restir ${restirFlags & RF.restir ? `on [${preset}]` : 'off'} | ` +
          `${scene.lightCount} light faces | ` +
          `bounces ${bounces} | denoise ${denoiseOn ? 'on' : 'off'} (F) | ` +
          `temporal ${temporalOn ? 'on' : 'off'} (T) | sun ${sunAnimate ? 'moving' : 'still'} (L)\n` +
          `click to capture mouse · WASD move · Space/Q up/down · Shift fast · ` +
          `1-6 bounce count · [ ] render scale`;
      }
    } catch (err) {
      fatal(err.stack || err);
      return;
    }
    if (stopAt > 0 && frame >= stopAt) { status.stopped = true; return; }
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
}

init().catch((err) => fatal(err.stack || err));
