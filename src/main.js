import { Camera } from './camera.js';
import { generateScene, VOXEL_SIZE, BRICK } from './scene.js';
import { normalize3 } from './math.js';

// ---------------------------------------------------------------------------
// Real-time path-traced global illumination for a 1/16 m voxel world.
// Pipeline per frame (all WGSL compute except the final blit):
//   1. pathtrace  — 1 spp, multi-bounce GI + sun NEE, demodulated output
//   2. temporal   — camera reprojection + exponential history accumulation
//   3. atrous ×3  — edge-aware wavelet denoise, relaxing as history grows
//   4. present    — remodulate albedo, expose, ACES tonemap
// ---------------------------------------------------------------------------

const params = new URLSearchParams(location.search);
const GRID = parseInt(params.get('grid') || '256', 10);
let renderScale = parseFloat(params.get('scale') || '1');
let bounces = parseInt(params.get('bounces') || '2', 10);
let exposure = 1.0;
let denoiseOn = params.get('denoise') !== '0';
let temporalOn = params.get('temporal') !== '0';
let sunAnimate = false;
let sunAzimuth = 0.9;
let sunElevation = 0.85;

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

async function init() {
  if (!navigator.gpu) throw new Error('WebGPU is not available in this browser.');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('No WebGPU adapter found.');
  const device = await adapter.requestDevice();
  device.addEventListener('uncapturederror', (e) => fatal(e.error.message));
  device.lost.then((info) => {
    if (info.reason !== 'destroyed') fatal(`WebGPU device lost: ${info.message}`);
  });
  console.log('adapter:', JSON.stringify({
    vendor: adapter.info?.vendor, architecture: adapter.info?.architecture,
    device: adapter.info?.device, description: adapter.info?.description,
  }));

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
  const scene = generateScene(GRID);
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

  const uniformBuf = device.createBuffer({
    label: 'uniforms', size: 256,
    usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST,
  });

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
  const voxelLib = await loadShader('voxel.wgsl');
  const [ptModule, tModule, aModule, prModule] = await Promise.all([
    makeModule(device, 'pathtrace', common + voxelLib + (await loadShader('pathtrace.wgsl'))),
    makeModule(device, 'temporal', common + (await loadShader('temporal.wgsl'))),
    makeModule(device, 'atrous', common + (await loadShader('atrous.wgsl'))),
    makeModule(device, 'present', await loadShader('present.wgsl')),
  ]);

  const ptPipeline = device.createComputePipeline({
    label: 'pathtrace', layout: 'auto', compute: { module: ptModule, entryPoint: 'main' },
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
  let rw = 0, rh = 0;

  function makeTex(label, format) {
    return device.createTexture({
      label, format, size: [rw, rh],
      usage: GPUTextureUsage.STORAGE_BINDING | GPUTextureUsage.TEXTURE_BINDING,
    }).createView();
  }

  function recreateTargets() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    canvas.width = Math.max(8, Math.floor(canvas.clientWidth * dpr));
    canvas.height = Math.max(8, Math.floor(canvas.clientHeight * dpr));
    rw = Math.max(8, Math.floor(canvas.width * renderScale));
    rh = Math.max(8, Math.floor(canvas.height * renderScale));
    tex = {
      radiance: makeTex('radiance', 'rgba16float'),
      albedo: makeTex('albedo', 'rgba8unorm'),
      gbuf: [makeTex('gbufA', 'rgba32float'), makeTex('gbufB', 'rgba32float')],
      accum: [makeTex('accumA', 'rgba16float'), makeTex('accumB', 'rgba16float')],
      denoise: [makeTex('denoiseA', 'rgba16float'), makeTex('denoiseB', 'rgba16float')],
    };
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
  const uniformData = new ArrayBuffer(256);
  const f32 = new Float32Array(uniformData);
  const u32 = new Uint32Array(uniformData);
  let prevViewProj = null;
  let prevCamPos = [...camera.pos];
  let frame = 0;

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
    u32.set([frame, bounces, temporalOn ? 1 : 0, 0], 48);
    f32.set([rw, rh, exposure, 0], 52);
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
    return { pt, tp, at, pr };
  }

  // --- Offscreen capture (used by the headless test; canvas presentation is
  // not composited in some headless environments, so this reads the rendered
  // image straight off the GPU) ---------------------------------------------
  let lastParity = 0;
  status.capture = async () => {
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
    return {
      png: c2.toDataURL('image/png'),
      mean, std: Math.sqrt(Math.max(0, sum2 / n - mean * mean)),
      w: rw, h: rh,
    };
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

      let pass = enc.beginComputePass({ label: 'pathtrace' });
      pass.setPipeline(ptPipeline);
      pass.setBindGroup(0, bg.pt);
      pass.dispatchWorkgroups(wg[0], wg[1]);
      pass.end();

      pass = enc.beginComputePass({ label: 'temporal' });
      pass.setPipeline(tPipeline);
      pass.setBindGroup(0, bg.tp);
      pass.dispatchWorkgroups(wg[0], wg[1]);
      pass.end();

      if (denoiseOn) {
        for (const group of bg.at) {
          pass = enc.beginComputePass({ label: 'atrous' });
          pass.setPipeline(aPipeline);
          pass.setBindGroup(0, group);
          pass.dispatchWorkgroups(wg[0], wg[1]);
          pass.end();
        }
      }

      if (ctx) {
        const rp = enc.beginRenderPass({
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

      device.queue.submit([enc.finish()]);
      if (checkErrors) {
        Promise.all([device.popErrorScope(), device.popErrorScope()]).then(([oom, val]) => {
          if (oom) fatal(`out-of-memory: ${oom.message}`);
          if (val) fatal(`validation: ${val.message}`);
        }).catch(() => { /* device lost; reported via device.lost */ });
      }

      lastParity = parity;
      frame++;
      status.frames = frame;
      status.ready = true;

      fpsAccum += dt; fpsCount++;
      if (fpsAccum >= 0.5) {
        fps = fpsCount / fpsAccum;
        fpsAccum = 0; fpsCount = 0;
        hud.textContent =
          `${fps.toFixed(0)} fps | ${rw}x${rh} (scale ${renderScale.toFixed(2)}) | ` +
          `grid ${GRID}³ @ ${(VOXEL_SIZE * 100).toFixed(2)} cm voxels | ` +
          `bounces ${bounces} | denoise ${denoiseOn ? 'on' : 'off'} (F) | ` +
          `temporal ${temporalOn ? 'on' : 'off'} (T) | sun ${sunAnimate ? 'moving' : 'still'} (L)\n` +
          `click to capture mouse · WASD move · Space/Q up/down · Shift fast · ` +
          `1-6 bounce count · [ ] render scale`;
      }
    } catch (err) {
      fatal(err.stack || err);
      return;
    }
    requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
}

init().catch((err) => fatal(err.stack || err));
