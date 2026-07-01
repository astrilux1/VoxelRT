// Minimal column-major mat4 helpers (matching WGSL mat4x4<f32> layout).

export function mat4Mul(a, b) {
  const out = new Float32Array(16);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      let s = 0;
      for (let k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      out[c * 4 + r] = s;
    }
  }
  return out;
}

// WebGPU clip space: z in [0, 1], right-handed view (camera looks down -Z).
export function perspective(fovYRad, aspect, near, far) {
  const f = 1 / Math.tan(fovYRad / 2);
  const m = new Float32Array(16);
  m[0] = f / aspect;
  m[5] = f;
  m[10] = far / (near - far);
  m[11] = -1;
  m[14] = (near * far) / (near - far);
  return m;
}

export function invPerspective(fovYRad, aspect, near, far) {
  const f = 1 / Math.tan(fovYRad / 2);
  const m = new Float32Array(16);
  m[0] = aspect / f;
  m[5] = 1 / f;
  m[11] = (near - far) / (near * far);
  m[14] = -1;
  m[15] = 1 / near;
  return m;
}

// Camera basis from yaw/pitch. yaw=0 faces +Z; positive pitch looks up.
export function cameraBasis(yaw, pitch) {
  const cp = Math.cos(pitch), sp = Math.sin(pitch);
  const cy = Math.cos(yaw), sy = Math.sin(yaw);
  const forward = [sy * cp, sp, cy * cp];
  const right = [-cy, 0, sy];   // forward x worldUp
  const up = [
    right[1] * forward[2] - right[2] * forward[1],
    right[2] * forward[0] - right[0] * forward[2],
    right[0] * forward[1] - right[1] * forward[0],
  ];
  return { forward, right, up };
}

// Rigid camera->world transform (columns: right, up, -forward, position).
export function camToWorld(pos, yaw, pitch) {
  const { forward, right, up } = cameraBasis(yaw, pitch);
  return new Float32Array([
    right[0], right[1], right[2], 0,
    up[0], up[1], up[2], 0,
    -forward[0], -forward[1], -forward[2], 0,
    pos[0], pos[1], pos[2], 1,
  ]);
}

// Analytic inverse of the rigid transform above (world->camera view matrix).
export function worldToCam(pos, yaw, pitch) {
  const { forward, right, up } = cameraBasis(yaw, pitch);
  const z = [-forward[0], -forward[1], -forward[2]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  return new Float32Array([
    right[0], up[0], z[0], 0,
    right[1], up[1], z[1], 0,
    right[2], up[2], z[2], 0,
    -dot(right, pos), -dot(up, pos), -dot(z, pos), 1,
  ]);
}

export function normalize3(v) {
  const l = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / l, v[1] / l, v[2] / l];
}
