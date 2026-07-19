// Generic exclusive prefix sum over u32, three dispatches:
//   scan_blocks:   per-256 workgroup scan, writes per-block sums
//   scan_sums:     single workgroup scans the block sums (supports up to
//                  256*256 = 65536 blocks -> 16.7M elements; 128^3 = 2.1M ok)
//   scan_add:      adds scanned block sums back
// Input is read-modify-write in place: buf[i] becomes exclusive prefix.

struct ScanParams { n : u32, _p0 : u32, _p1 : u32, _p2 : u32 };
@group(0) @binding(0) var<uniform> sp : ScanParams;
@group(0) @binding(1) var<storage, read_write> buf : array<u32>;
@group(0) @binding(2) var<storage, read_write> blockSums : array<u32>;

var<workgroup> tile : array<u32, 256>;

// Hillis-Steele inclusive scan of tile[] in shared memory. 4-element trace
// of [a, b, c, d]:
//   offset 1 -> [a, a+b, b+c, c+d]
//   offset 2 -> [a, a+b, a+b+c, a+b+c+d]   (inclusive prefix)
// Callers derive the exclusive prefix as inclusive - original element.
// The trailing barrier makes every tile[li] visible to all threads on return.
fn wg_inclusive_scan(li : u32) {
  var offset = 1u;
  loop {
    if (offset >= 256u) { break; }
    workgroupBarrier();
    var v = tile[li];
    if (li >= offset) { v += tile[li - offset]; }
    workgroupBarrier();
    tile[li] = v;
    offset = offset << 1u;
  }
  workgroupBarrier();
}

@compute @workgroup_size(256)
fn scan_blocks(@builtin(global_invocation_id) gid : vec3<u32>,
               @builtin(local_invocation_id) l : vec3<u32>,
               @builtin(workgroup_id) wid : vec3<u32>) {
  let li = l.x;
  let i = gid.x;
  let x = select(0u, buf[i], i < sp.n);
  tile[li] = x;
  wg_inclusive_scan(li);
  let inclusive = tile[li];
  if (i < sp.n) { buf[i] = inclusive - x; }        // exclusive
  // Zero-padding above means thread 255's inclusive value is the block
  // total even when the tail block is partially filled.
  if (li == 255u) { blockSums[wid.x] = inclusive; }
}

@compute @workgroup_size(256)
fn scan_sums(@builtin(local_invocation_id) l : vec3<u32>) {
  let li = l.x;
  let nBlocks = (sp.n + 255u) / 256u;
  // Serial-in-parallel: scan 256-block chunks in sequence, carrying the
  // running total between chunks through tile[0].
  var base = 0u;
  var chunk = 0u;
  loop {
    let idx = chunk * 256u + li;
    let x = select(0u, blockSums[idx], idx < nBlocks);
    tile[li] = x;
    wg_inclusive_scan(li);
    let inclusive = tile[li];
    if (idx < nBlocks) { blockSums[idx] = base + inclusive - x; }
    // Carry invariant: every thread reads its inclusive value before tile[0]
    // is overwritten, and every thread reads tile[0] before the next chunk
    // overwrites tile[] — a barrier is required on each side of both writes.
    workgroupBarrier();
    if (li == 255u) { tile[0] = base + inclusive; }
    workgroupBarrier();
    base = tile[0];
    chunk += 1u;
    if (chunk * 256u >= nBlocks) { break; }
    workgroupBarrier();
  }
}

@compute @workgroup_size(256)
fn scan_add(@builtin(global_invocation_id) gid : vec3<u32>,
            @builtin(workgroup_id) wid : vec3<u32>) {
  if (gid.x < sp.n) { buf[gid.x] += blockSums[wid.x]; }
}
