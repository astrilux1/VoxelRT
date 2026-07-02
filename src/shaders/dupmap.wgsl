// ---------------------------------------------------------------------------
// dupmap.wgsl — sample duplication map (Lin 2026 §5).
//
// Counts how many reservoirs in the surrounding window carry a sample
// resampled from the same initial candidate (identical seed). The resulting
// score D in [0,1] drives adaptive cCap reduction during next frame's
// temporal reuse, breaking up correlation blobs before they persist.
// Window is 9x9 (the paper uses 17x17 at 1080p; this renderer runs at lower
// internal resolutions where 9x9 covers a comparable image fraction).
// ---------------------------------------------------------------------------

@group(0) @binding(9) var<storage, read> reservoirsB : array<Reservoir>;
@group(0) @binding(14) var dupOut : texture_storage_2d<r32float, write>;

const DUP_HALF : i32 = 4;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let dims = vec2<u32>(u32(u.params1.x), u32(u.params1.y));
  if (gid.x >= dims.x || gid.y >= dims.y) { return; }
  let pix = vec2<i32>(gid.xy);
  let idims = vec2<i32>(dims);

  let mine = reservoirsB[gid.y * dims.x + gid.x];
  let myKind = (mine.data.y >> 16u) & 3u;
  if (myKind == SK_NONE) {
    textureStore(dupOut, pix, vec4<f32>(0.0));
    return;
  }

  var count = 0;
  for (var dy = -DUP_HALF; dy <= DUP_HALF; dy++) {
    for (var dx = -DUP_HALF; dx <= DUP_HALF; dx++) {
      if (dx == 0 && dy == 0) { continue; }
      let q = pix + vec2<i32>(dx, dy);
      if (any(q < vec2<i32>(0)) || any(q >= idims)) { continue; }
      let r = reservoirsB[u32(q.y) * dims.x + u32(q.x)];
      if (((r.data.y >> 16u) & 3u) != SK_NONE && r.data.w == mine.data.w) {
        count++;
      }
    }
  }

  let win = f32((2 * DUP_HALF + 1) * (2 * DUP_HALF + 1) - 1);
  textureStore(dupOut, pix, vec4<f32>(f32(count) / win));
}
