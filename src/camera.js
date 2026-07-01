import { perspective, invPerspective, camToWorld, worldToCam, mat4Mul, cameraBasis } from './math.js';

// Fly camera: pointer-lock mouse look + WASD/EQ movement.
export class Camera {
  constructor(canvas, spawn) {
    this.canvas = canvas;
    this.pos = [...spawn.pos];
    this.yaw = spawn.yaw;
    this.pitch = spawn.pitch;
    this.fov = (70 * Math.PI) / 180;
    this.near = 0.05;
    this.far = 400;
    this.moved = true;
    this.keys = new Set();

    canvas.addEventListener('click', () => canvas.requestPointerLock());
    document.addEventListener('mousemove', (e) => {
      if (document.pointerLockElement !== canvas) return;
      this.yaw -= e.movementX * 0.0022;
      this.pitch -= e.movementY * 0.0022;
      const lim = Math.PI / 2 - 0.01;
      this.pitch = Math.max(-lim, Math.min(lim, this.pitch));
      this.moved = true;
    });
    window.addEventListener('keydown', (e) => this.keys.add(e.code));
    window.addEventListener('keyup', (e) => this.keys.delete(e.code));
    window.addEventListener('blur', () => this.keys.clear());
  }

  update(dt) {
    const { forward, right } = cameraBasis(this.yaw, this.pitch);
    const speed = (this.keys.has('ShiftLeft') ? 8 : 2.5) * dt;
    let d = [0, 0, 0];
    const add = (v, s) => { d[0] += v[0] * s; d[1] += v[1] * s; d[2] += v[2] * s; };
    if (this.keys.has('KeyW')) add(forward, speed);
    if (this.keys.has('KeyS')) add(forward, -speed);
    if (this.keys.has('KeyD')) add(right, speed);
    if (this.keys.has('KeyA')) add(right, -speed);
    if (this.keys.has('KeyE') || this.keys.has('Space')) add([0, 1, 0], speed);
    if (this.keys.has('KeyQ')) add([0, 1, 0], -speed);
    if (d[0] || d[1] || d[2]) {
      this.pos[0] += d[0]; this.pos[1] += d[1]; this.pos[2] += d[2];
      this.moved = true;
    }
  }

  matrices(aspect) {
    const proj = perspective(this.fov, aspect, this.near, this.far);
    const view = worldToCam(this.pos, this.yaw, this.pitch);
    const viewProj = mat4Mul(proj, view);
    const invViewProj = mat4Mul(
      camToWorld(this.pos, this.yaw, this.pitch),
      invPerspective(this.fov, aspect, this.near, this.far)
    );
    return { viewProj, invViewProj };
  }
}
