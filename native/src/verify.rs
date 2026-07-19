// CPU reference builders (docs/S64.md §3 invariant 3): build both
// acceleration structures from the fixture voxel array on the CPU,
// byte-compatible with the GPU layouts, and byte-compare every structure
// buffer in --verify-build mode.
//
// Fixture-only by design: a sparse1024 CPU reference would require bit-exact
// f32 value-noise reproduction across CPU and GPU (rounding of mix chains is
// not portable), so sparse1024 is covered by determinism invariant 2 (two
// GPU builds must be byte-identical) instead. Fixture voxels are plain u32s,
// so the CPU walk is exact.

use crate::{build_brickmap, load_scene, readback_u32, s64, Args, Gpu, FIXTURE_GRID};

const NONE: u32 = 0xffff_ffff;
const BGRID: usize = 128; // brick grid: 1024 / 8
const L1GRID: usize = 64; // L1 grid: 1024 / 16
const CGRID: usize = 16; // chunk grid: 1024 / 64

/// In-place exclusive prefix sum; returns the total (= gpu_scan's contract).
fn excl_scan(v: &mut [u32]) -> u32 {
    let mut acc = 0u32;
    for x in v.iter_mut() {
        let n = *x;
        *x = acc;
        acc += n;
    }
    acc
}

/// Set bits strictly below bit b (mirror of the WGSL popcnt_below).
fn popcnt_below(m: u64, b: u32) -> u32 {
    (m & ((1u64 << b) - 1)).count_ones()
}

struct CpuBrickmap {
    ptr: Vec<u32>,
    masks: Vec<u32>, // 16 words per allocated brick
    mats: Vec<u32>,  // 512 words per allocated brick
}

/// Mirror of brickmap_build.wgsl: slots are the exclusive scan of brick
/// occupancy in linear brick index order (x + y*128 + z*128^2); local voxel
/// bit/mat index is li = x + y*8 + z*64.
fn cpu_brickmap(vox: &impl Fn(usize, usize, usize) -> u32) -> CpuBrickmap {
    let active = (FIXTURE_GRID / 8) as usize; // bricks beyond this are air
    let mut ptr = vec![NONE; BGRID * BGRID * BGRID];
    let mut masks = Vec::new();
    let mut mats = Vec::new();
    let mut slot = 0u32;
    for bz in 0..active {
        for by in 0..active {
            for bx in 0..active {
                let mut mask = [0u32; 16];
                let mut m512 = [0u32; 512];
                let mut any = false;
                for li in 0..512usize {
                    let (x, y, z) = (li & 7, (li >> 3) & 7, li >> 6);
                    let m = vox(bx * 8 + x, by * 8 + y, bz * 8 + z);
                    m512[li] = m;
                    if m != 0 {
                        mask[li >> 5] |= 1 << (li & 31);
                        any = true;
                    }
                }
                if any {
                    ptr[bx + by * BGRID + bz * BGRID * BGRID] = slot;
                    masks.extend_from_slice(&mask);
                    mats.extend_from_slice(&m512);
                    slot += 1;
                }
            }
        }
    }
    CpuBrickmap { ptr, masks, mats }
}

struct CpuS64 {
    tlas: Vec<u32>,
    nodes: Vec<u32>,  // 4 words per node
    leaves: Vec<u32>, // 4 words per leaf
    mats: Vec<u32>,
    root_count: u32,
    l1_count: u32,
    leaf_count: u32,
    mat_count: u32,
}

/// Mirror of s64_build.wgsl (see its header for the layout): all allocation
/// bases are exclusive scans in linear cell index order; sibling ranks are
/// popcounts below the child's bit (bit = x + y*4 + z*16 inside any 4^3
/// tile); matBase is the scanned per-L1 total plus the in-cell exclusive
/// prefix of leaf solid counts in bit order.
fn cpu_s64(vox: &impl Fn(usize, usize, usize) -> u32) -> CpuS64 {
    let al = (FIXTURE_GRID / 4) as usize; // leaf cells beyond this are air

    // Leaf masks over the active region (voxel bit = x + y*4 + z*16).
    let mut leaf_masks = vec![0u64; al * al * al];
    for lz in 0..al {
        for ly in 0..al {
            for lx in 0..al {
                let mut m = 0u64;
                for b in 0..64u32 {
                    let (x, y, z) = ((b & 3) as usize, ((b >> 2) & 3) as usize, (b >> 4) as usize);
                    if vox(lx * 4 + x, ly * 4 + y, lz * 4 + z) != 0 {
                        m |= 1u64 << b;
                    }
                }
                leaf_masks[lx + ly * al + lz * al * al] = m;
            }
        }
    }
    // Global leaf coords (0..256 per axis) -> mask, zero outside the fixture.
    let leaf_mask_at = |lx: usize, ly: usize, lz: usize| -> u64 {
        if lx < al && ly < al && lz < al { leaf_masks[lx + ly * al + lz * al * al] } else { 0 }
    };

    // pass_l1: occupancy mask + child/mat counts per L1 cell.
    let n_l1 = L1GRID * L1GRID * L1GRID;
    let mut l1_mask = vec![0u64; n_l1];
    let mut l1_child = vec![0u32; n_l1];
    let mut l1_mat = vec![0u32; n_l1];
    for i in 0..n_l1 {
        let (cx, cy, cz) = (i % L1GRID, (i / L1GRID) % L1GRID, i / (L1GRID * L1GRID));
        let mut m = 0u64;
        let mut matc = 0u32;
        for b in 0..64u32 {
            let (x, y, z) = ((b & 3) as usize, ((b >> 2) & 3) as usize, (b >> 4) as usize);
            let lm = leaf_mask_at(cx * 4 + x, cy * 4 + y, cz * 4 + z);
            if lm != 0 {
                m |= 1u64 << b;
                matc += lm.count_ones();
            }
        }
        l1_mask[i] = m;
        l1_child[i] = m.count_ones();
        l1_mat[i] = matc;
    }

    // pass_chunk: chunk root mask + counts.
    let n_chunks = CGRID * CGRID * CGRID;
    let mut chunk_mask = vec![0u64; n_chunks];
    let mut root_child = vec![0u32; n_chunks];
    let mut chunk_occ = vec![0u32; n_chunks];
    for ci in 0..n_chunks {
        let (cx, cy, cz) = (ci % CGRID, (ci / CGRID) % CGRID, ci / (CGRID * CGRID));
        let mut m = 0u64;
        for b in 0..64u32 {
            let (x, y, z) = ((b & 3) as usize, ((b >> 2) & 3) as usize, (b >> 4) as usize);
            let li = (cx * 4 + x) + (cy * 4 + y) * L1GRID + (cz * 4 + z) * L1GRID * L1GRID;
            if l1_child[li] != 0 {
                m |= 1u64 << b;
            }
        }
        chunk_mask[ci] = m;
        root_child[ci] = m.count_ones();
        chunk_occ[ci] = u32::from(m != 0);
    }

    // pass_tlas: root mask over 4^3 chunk-groups + per-group chunk masks.
    let mut tlas = vec![0u32; 2 + 128 + n_chunks];
    let mut root_mask = 0u64;
    for g in 0..64u32 {
        let (gx, gy, gz) = ((g & 3) as usize, ((g >> 2) & 3) as usize, (g >> 4) as usize);
        let mut m = 0u64;
        for c in 0..64u32 {
            let (x, y, z) = ((c & 3) as usize, ((c >> 2) & 3) as usize, (c >> 4) as usize);
            let ci = (gx * 4 + x) + (gy * 4 + y) * CGRID + (gz * 4 + z) * CGRID * CGRID;
            if chunk_occ[ci] != 0 {
                m |= 1u64 << c;
            }
        }
        tlas[2 + g as usize * 2] = m as u32;
        tlas[3 + g as usize * 2] = (m >> 32) as u32;
        if m != 0 {
            root_mask |= 1u64 << g;
        }
    }
    tlas[0] = root_mask as u32;
    tlas[1] = (root_mask >> 32) as u32;

    // Allocation scans (the four gpu_scan calls).
    let leaf_count = excl_scan(&mut l1_child);
    let mat_count = excl_scan(&mut l1_mat);
    let l1_count = excl_scan(&mut root_child);
    let root_count = excl_scan(&mut chunk_occ);

    // pass_roots + pass_l1_write.
    let mut nodes = vec![0u32; (root_count + l1_count) as usize * 4];
    for ci in 0..n_chunks {
        let m = chunk_mask[ci];
        if m != 0 {
            let slot = chunk_occ[ci] as usize;
            nodes[slot * 4..slot * 4 + 4]
                .copy_from_slice(&[m as u32, (m >> 32) as u32, root_count + root_child[ci], 0]);
            tlas[130 + ci] = slot as u32;
        } else {
            tlas[130 + ci] = NONE;
        }
    }
    for i in 0..n_l1 {
        let m = l1_mask[i];
        if m == 0 {
            continue;
        }
        let (cx, cy, cz) = (i % L1GRID, (i / L1GRID) % L1GRID, i / (L1GRID * L1GRID));
        let ci = (cx / 4) + (cy / 4) * CGRID + (cz / 4) * CGRID * CGRID;
        let bit = ((cx & 3) + (cy & 3) * 4 + (cz & 3) * 16) as u32;
        let slot = (root_count + root_child[ci] + popcnt_below(chunk_mask[ci], bit)) as usize;
        nodes[slot * 4..slot * 4 + 4].copy_from_slice(&[m as u32, (m >> 32) as u32, l1_child[i], 0]);
    }

    // pass_matbase + pass_leaf_fill.
    let mut leaves = vec![0u32; leaf_count as usize * 4];
    let mut mats = vec![0u32; mat_count as usize];
    for i in 0..n_l1 {
        let (cx, cy, cz) = (i % L1GRID, (i / L1GRID) % L1GRID, i / (L1GRID * L1GRID));
        let mut mat_base = l1_mat[i];
        for b in 0..64u32 {
            let (x, y, z) = ((b & 3) as usize, ((b >> 2) & 3) as usize, (b >> 4) as usize);
            let (lx, ly, lz) = (cx * 4 + x, cy * 4 + y, cz * 4 + z);
            let lm = leaf_mask_at(lx, ly, lz);
            if lm != 0 {
                let slot = (l1_child[i] + popcnt_below(l1_mask[i], b)) as usize;
                leaves[slot * 4..slot * 4 + 4]
                    .copy_from_slice(&[lm as u32, (lm >> 32) as u32, mat_base, 0]);
                let mut k = mat_base as usize;
                for v in 0..64u32 {
                    if lm & (1u64 << v) != 0 {
                        let (vx, vy, vz) = ((v & 3) as usize, ((v >> 2) & 3) as usize, (v >> 4) as usize);
                        mats[k] = vox(lx * 4 + vx, ly * 4 + vy, lz * 4 + vz);
                        k += 1;
                    }
                }
            }
            mat_base += lm.count_ones();
        }
    }

    CpuS64 { tlas, nodes, leaves, mats, root_count, l1_count, leaf_count, mat_count }
}

/// Read back a GPU buffer and word-compare it against the CPU reference.
/// Returns the mismatch count; the first few mismatches are printed.
fn compare(gpu: &Gpu, label: &str, buf: &wgpu::Buffer, cpu: &[u32]) -> u64 {
    let gpu_words = readback_u32(gpu, buf, 0, cpu.len());
    let mut mism = 0u64;
    for (i, (&g, &c)) in gpu_words.iter().zip(cpu).enumerate() {
        if g != c {
            if mism < 5 {
                eprintln!("  {label}[{i}]: gpu {g:#010x} != cpu {c:#010x}");
            }
            mism += 1;
        }
    }
    if mism == 0 {
        eprintln!("  {label}: OK ({} words)", cpu.len());
    } else {
        eprintln!("  {label}: {mism} mismatched words / {}", cpu.len());
    }
    mism
}

fn check_count(label: &str, gpu: u32, cpu: u32) -> u64 {
    if gpu == cpu {
        0
    } else {
        eprintln!("  {label}: gpu {gpu} != cpu {cpu}");
        1
    }
}

/// --verify-build: build both structures on GPU and CPU from fixture256 and
/// byte-compare every structure buffer. Exits non-zero on any mismatch.
pub fn run_verify_build(gpu: &Gpu, args: &Args, shader_dir: &str) -> ! {
    if args.scene != "fixture256" {
        eprintln!("verify-build: fixture256 only (sparse1024 noise is not CPU-reproducible; ignoring --scene {})", args.scene);
    }
    let scene = load_scene(gpu, "fixture256");
    let voxels = scene.voxels.as_ref().expect("fixture voxels");
    let n = FIXTURE_GRID as usize;
    let vox = |x: usize, y: usize, z: usize| -> u32 {
        if x < n && y < n && z < n { voxels[x + y * n + z * n * n] } else { 0 }
    };

    let mut mism = 0u64;

    let bm = build_brickmap(gpu, shader_dir, args.seed, &scene);
    let cpu_bm = cpu_brickmap(&vox);
    let cpu_bricks = (cpu_bm.masks.len() / 16) as u32;
    eprintln!("verify-build[brickmap]: gpu {} bricks, cpu {cpu_bricks} bricks", bm.brick_count);
    mism += check_count("brickCount", bm.brick_count, cpu_bricks);
    if mism == 0 {
        mism += compare(gpu, "brickPtr", &bm.brick_ptr, &cpu_bm.ptr);
        mism += compare(gpu, "brickMasks", &bm.brick_masks, &cpu_bm.masks);
        mism += compare(gpu, "brickMats", &bm.brick_mats, &cpu_bm.mats);
    }

    let sb = s64::build_s64(gpu, shader_dir, args.seed, &scene);
    let cpu_s = cpu_s64(&vox);
    eprintln!(
        "verify-build[s64]: gpu {} roots, {} l1, {} leaves, {} mats",
        sb.root_count, sb.l1_count, sb.leaf_count, sb.mat_count
    );
    let s64_counts = check_count("rootCount", sb.root_count, cpu_s.root_count)
        + check_count("l1Count", sb.l1_count, cpu_s.l1_count)
        + check_count("leafCount", sb.leaf_count, cpu_s.leaf_count)
        + check_count("matCount", sb.mat_count, cpu_s.mat_count);
    mism += s64_counts;
    if s64_counts == 0 {
        mism += compare(gpu, "tlas", &sb.tlas, &cpu_s.tlas);
        mism += compare(gpu, "nodes", &sb.nodes, &cpu_s.nodes);
        mism += compare(gpu, "leaves", &sb.leaves, &cpu_s.leaves);
        mism += compare(gpu, "mats", &sb.mats, &cpu_s.mats);
    }

    if mism > 0 {
        eprintln!("verify-build FAILED ({mism} mismatches)");
        std::process::exit(1);
    }
    eprintln!("verify-build PASSED");
    std::process::exit(0);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Decode voxel p from the CPU S64 structure exactly the way
    /// s64_trace.wgsl resolves references (TLAS chunkRoots -> root node ->
    /// L1 node -> leaf -> popcount-compacted material). 0 = air.
    fn s64_voxel(s: &CpuS64, x: usize, y: usize, z: usize) -> u32 {
        let ci = x / 64 + (y / 64) * CGRID + (z / 64) * CGRID * CGRID;
        let slot = s.tlas[130 + ci];
        if slot == NONE {
            return 0;
        }
        let root = &s.nodes[slot as usize * 4..slot as usize * 4 + 4];
        let rmask = root[0] as u64 | (root[1] as u64) << 32;
        let rbit = ((x / 16) & 3) + ((y / 16) & 3) * 4 + ((z / 16) & 3) * 16;
        if rmask & (1u64 << rbit) == 0 {
            return 0;
        }
        let l1i = (root[2] + popcnt_below(rmask, rbit as u32)) as usize;
        let l1 = &s.nodes[l1i * 4..l1i * 4 + 4];
        let lmask = l1[0] as u64 | (l1[1] as u64) << 32;
        let lbit = ((x / 4) & 3) + ((y / 4) & 3) * 4 + ((z / 4) & 3) * 16;
        if lmask & (1u64 << lbit) == 0 {
            return 0;
        }
        let li = (l1[2] + popcnt_below(lmask, lbit as u32)) as usize;
        let leaf = &s.leaves[li * 4..li * 4 + 4];
        let vmask = leaf[0] as u64 | (leaf[1] as u64) << 32;
        let vbit = (x & 3) + (y & 3) * 4 + (z & 3) * 16;
        if vmask & (1u64 << vbit) == 0 {
            return 0;
        }
        s.mats[(leaf[2] + popcnt_below(vmask, vbit as u32)) as usize]
    }

    /// Decode voxel p from the CPU brickmap the way brickmap_trace.wgsl does.
    fn brickmap_voxel(b: &CpuBrickmap, x: usize, y: usize, z: usize) -> u32 {
        let slot = b.ptr[x / 8 + (y / 8) * BGRID + (z / 8) * BGRID * BGRID];
        if slot == NONE {
            return 0;
        }
        let li = (x & 7) + (y & 7) * 8 + (z & 7) * 64;
        if b.masks[slot as usize * 16 + (li >> 5)] & (1 << (li & 31)) == 0 {
            return 0;
        }
        b.mats[slot as usize * 512 + li]
    }

    /// CPU-only (no GPU): both reference structures must decode back to
    /// exactly the fixture voxel array, and the S64 TLAS masks must agree
    /// with the chunkRoots table.
    #[test]
    fn cpu_references_decode_to_fixture() {
        let path = crate::fixture_path();
        assert!(path.exists(), "run `node test/dump-scene.mjs` to generate {}", path.display());
        let voxels = crate::load_fixture(&path);
        let n = FIXTURE_GRID as usize;
        let vox = |x: usize, y: usize, z: usize| -> u32 {
            if x < n && y < n && z < n { voxels[x + y * n + z * n * n] } else { 0 }
        };
        let bm = cpu_brickmap(&vox);
        let s = cpu_s64(&vox);

        let solid: u32 = voxels.iter().map(|&m| u32::from(m != 0)).sum();
        assert_eq!(s.mat_count as usize, s.mats.len());
        assert_eq!(s.mat_count, solid, "S64 material count != fixture solid count");

        // TLAS root/group masks vs chunkRoots (the trace descends via masks
        // and only dereferences chunkRoots for set bits).
        let root_mask = s.tlas[0] as u64 | (s.tlas[1] as u64) << 32;
        for ci in 0..CGRID * CGRID * CGRID {
            let (cx, cy, cz) = (ci % CGRID, (ci / CGRID) % CGRID, ci / (CGRID * CGRID));
            let g = cx / 4 + (cy / 4) * 4 + (cz / 4) * 16;
            let gmask = s.tlas[2 + g * 2] as u64 | (s.tlas[3 + g * 2] as u64) << 32;
            let cbit = (cx & 3) + (cy & 3) * 4 + (cz & 3) * 16;
            let occupied = s.tlas[130 + ci] != NONE;
            assert_eq!(gmask & (1u64 << cbit) != 0, occupied, "group mask vs chunkRoots at chunk {ci}");
            if occupied {
                assert!(root_mask & (1u64 << g) != 0, "root mask missing group {g}");
            }
        }

        // Every voxel in the 1024^3 world that could be non-air lives in the
        // fixture corner; sample the corner exhaustively plus a shell beyond.
        for z in 0..n {
            for y in 0..n {
                for x in 0..n {
                    let want = vox(x, y, z);
                    assert_eq!(brickmap_voxel(&bm, x, y, z), want, "brickmap voxel ({x},{y},{z})");
                    assert_eq!(s64_voxel(&s, x, y, z), want, "s64 voxel ({x},{y},{z})");
                }
            }
        }
        for &p in &[n, n + 1, 511, 1023] {
            assert_eq!(brickmap_voxel(&bm, p, 0, 0), 0);
            assert_eq!(s64_voxel(&s, 0, p, 0), 0);
        }
    }

    // ===== docs/S64OPT.md CPU cross-check ==================================
    //
    // 1:1 Rust ports of the traversal logic in
    // shaders/s64_trace_stackless_opt.wgsl, traced over the CPU-built S64
    // structure with a deterministic >=10k-ray bundle (axis-aligned,
    // diagonal, grazing, random, short-maxT). What IS covered:
    //   - opt levers vs control decisions: memo / skip / anyhit are asserted
    //     BITWISE-identical (t bits, hit voxel, material, normal) to the
    //     all-off port, which with all levers off is line-for-line the
    //     promoted control kernel;
    //   - mirror is asserted equal within the identity-gate v2 tolerance
    //     (hit/miss + material + voxel equality; |dt| <= max(1e-4 voxel,
    //     16 ULP)) under a small knife-edge budget — the mirror transform
    //     (GRID - ro) legitimately rounds;
    //   - the control port itself is anchored against an independent f64
    //     voxel-marching oracle over the raw fixture voxels;
    //   - helper properties: row-mask emptiness vs brute force, mirrored
    //     bit-index round-trip, memo containment tests.
    // What is NOT covered here: the WGSL text itself executing (naga
    // validates it via --check-shaders; the Rust ports mirror it expression
    // by expression but are a transcription), GPU fp contraction (fma) and
    // subgroup scheduling, and the shader<->host module wiring — those need
    // the deferred GPU gate/bench runs.

    use std::array::from_fn;
    use std::sync::OnceLock;

    const GRID_T: i32 = 1024;
    const VOXEL_SIZE: f32 = 0.0625;
    const INV_VOXEL: f32 = 16.0;

    #[derive(Clone, Copy, Default)]
    struct Levers {
        memo: bool,
        mirror: bool,
        skip: bool,
        anyhit: bool,
    }

    #[derive(Clone, Copy, Default)]
    struct TraceStats {
        root_descents: u64,
        memo_entries: u64,
        jumps: u64,
    }

    #[derive(Clone, Copy, Debug)]
    struct PortHit {
        t: f32,
        n: [f32; 3],
        mat: u32,
        vox: [i32; 3], // hit voxel, un-mirrored coords; [-1; 3] on miss
    }

    fn fixture_s64() -> &'static (Vec<u32>, CpuS64) {
        static CELL: OnceLock<(Vec<u32>, CpuS64)> = OnceLock::new();
        CELL.get_or_init(|| {
            let path = crate::fixture_path();
            assert!(path.exists(), "run `node test/dump-scene.mjs` to generate {}", path.display());
            let voxels = crate::load_fixture(&path);
            let n = FIXTURE_GRID as usize;
            let s = {
                let vox = |x: usize, y: usize, z: usize| -> u32 {
                    if x < n && y < n && z < n { voxels[x + y * n + z * n * n] } else { 0 }
                };
                cpu_s64(&vox)
            };
            (voxels, s)
        })
    }

    fn fixture_voxel(voxels: &[u32], x: i32, y: i32, z: i32) -> u32 {
        let n = FIXTURE_GRID as i32;
        if (0..n).contains(&x) && (0..n).contains(&y) && (0..n).contains(&z) {
            voxels[(x + y * n + z * n * n) as usize]
        } else {
            0
        }
    }

    fn node4(s: &CpuS64, idx: usize) -> (u64, u32) {
        (s.nodes[idx * 4] as u64 | (s.nodes[idx * 4 + 1] as u64) << 32, s.nodes[idx * 4 + 2])
    }
    fn leaf4(s: &CpuS64, idx: usize) -> (u64, u32) {
        (s.leaves[idx * 4] as u64 | (s.leaves[idx * 4 + 1] as u64) << 32, s.leaves[idx * 4 + 2])
    }

    // --- WGSL helper ports (expression-for-expression) ---------------------

    fn wgsl_sign(v: f32) -> i32 {
        if v > 0.0 {
            1
        } else if v < 0.0 {
            -1
        } else {
            0
        }
    }

    fn safe_inv(v: [f32; 3]) -> [f32; 3] {
        from_fn(|i| {
            let s = if v[i] >= 0.0 { 1.0f32 } else { -1.0 };
            s / v[i].abs().max(1e-8)
        })
    }

    /// Port of the opt kernel's cell_bit(): un-mirrored 4^3 bit index.
    fn cell_bit(cell: [i32; 3], mir: [bool; 3]) -> u32 {
        let cu: [i32; 3] = from_fn(|i| if mir[i] { 3 - cell[i] } else { cell[i] });
        (cu[0] + cu[1] * 4 + cu[2] * 16) as u32
    }

    /// Port of the opt kernel's row_empty() on the (lo, hi) mask words.
    fn row_empty(m: (u32, u32), axis: usize, cu: [i32; 3]) -> bool {
        if axis == 0 {
            let s = (cu[1] * 4 + cu[2] * 16) as u32;
            if s < 32 {
                return (m.0 >> s) & 0xf == 0;
            }
            return (m.1 >> (s - 32)) & 0xf == 0;
        }
        if axis == 1 {
            let s = (cu[0] + (cu[2] & 1) * 16) as u32;
            let w = if cu[2] >= 2 { m.1 } else { m.0 };
            return (w >> s) & 0x1111 == 0;
        }
        let s = (cu[0] + cu[1] * 4) as u32;
        (m.0 >> s) & 0x10001 == 0 && (m.1 >> s) & 0x10001 == 0
    }

    /// Port of the opt kernel's axis_selected() (control step tie-break).
    fn axis_selected(axis: usize, t_cand: f32, t_max: [f32; 3]) -> bool {
        match axis {
            0 => t_cand <= t_max[1] && t_cand <= t_max[2],
            1 => t_cand < t_max[0] && t_cand <= t_max[2],
            _ => t_cand < t_max[0] && t_cand < t_max[1],
        }
    }

    // --- Port of the opt kernel's trace() ----------------------------------
    // With all levers false this is line-for-line the promoted control
    // kernel (s64_trace_stackless.wgsl); the lever branches mirror
    // s64_trace_stackless_opt.wgsl.
    fn trace_port(
        s: &CpuS64,
        ro_world: [f32; 3],
        rd: [f32; 3],
        max_t_world: f32,
        lv: Levers,
        stats: &mut TraceStats,
    ) -> PortHit {
        let mut hit = PortHit { t: -1.0, n: from_fn(|i| -rd[i]), mat: 0, vox: [-1; 3] };

        let ro0: [f32; 3] = from_fn(|i| ro_world[i] * INV_VOXEL);
        let max_t = max_t_world * INV_VOXEL;

        let mir: [bool; 3] = if lv.mirror { from_fn(|i| rd[i] < 0.0) } else { [false; 3] };
        let ro: [f32; 3] = from_fn(|i| if mir[i] { -ro0[i] } else { ro0[i] });
        let rdt: [f32; 3] = from_fn(|i| if mir[i] { -rd[i] } else { rd[i] });
        // Grid box in traversal space: [glo, ghi) = [0, GRID) or [-GRID, 0).
        let glo: [i32; 3] = from_fn(|i| if mir[i] { -GRID_T } else { 0 });
        let ghi: [i32; 3] = from_fn(|i| glo[i] + GRID_T);

        let inv_rd = safe_inv(rdt);
        let step_dir: [i32; 3] = from_fn(|i| wgsl_sign(rdt[i]));
        let step_f: [f32; 3] = from_fn(|i| step_dir[i] as f32);
        let step_ge: [f32; 3] = from_fn(|i| if rdt[i] >= 0.0 { 1.0 } else { 0.0 });
        let step_pos: [bool; 3] = from_fn(|i| step_dir[i] > 0);
        let step_out: [f32; 3] = from_fn(|i| if mir[i] { -step_f[i] } else { step_f[i] });

        let t0: [f32; 3] = from_fn(|i| (glo[i] as f32 - ro[i]) * inv_rd[i]);
        let t1: [f32; 3] = from_fn(|i| (ghi[i] as f32 - ro[i]) * inv_rd[i]);
        let tmin3: [f32; 3] = from_fn(|i| t0[i].min(t1[i]));
        let tmax3: [f32; 3] = from_fn(|i| t0[i].max(t1[i]));
        let t_enter = tmin3[0].max(tmin3[1]).max(tmin3[2].max(0.0));
        let t_exit = tmax3[0].min(tmax3[1]).min(tmax3[2].min(max_t));
        if t_enter > t_exit {
            return hit;
        }

        let mut amask: [f32; 3] = if tmin3[0] >= tmin3[1] && tmin3[0] >= tmin3[2] {
            [1.0, 0.0, 0.0]
        } else if tmin3[1] >= tmin3[2] {
            [0.0, 1.0, 0.0]
        } else {
            [0.0, 0.0, 1.0]
        };

        let mut t = t_enter;
        let mut vox: [i32; 3] =
            from_fn(|i| ((ro[i] + rdt[i] * (t + 1e-4)).floor() as i32).clamp(glo[i], ghi[i] - 1));

        let mut dom = 0usize;
        if lv.skip {
            let ard: [f32; 3] = from_fn(|i| rd[i].abs());
            if ard[1] > ard[0] && ard[1] >= ard[2] {
                dom = 1;
            } else if ard[2] > ard[0] && ard[2] > ard[1] {
                dom = 2;
            }
        }

        let mut have_chunk = false;
        let mut have_l1 = false;
        let mut chunk_org = [0i32; 3];
        let mut chunk_mask = 0u64;
        let mut chunk_base = 0u32;
        let mut l1_org = [0i32; 3];
        let mut l1_mask = 0u64;
        let mut l1_base = 0u32;

        for _iter in 0..8192 {
            let mut level: i32 = 0;
            let mut sh: u32 = 8;
            let mut origin = glo;
            let mut mask = s.tlas[0] as u64 | (s.tlas[1] as u64) << 32;
            let mut base: u32 = 0;
            if lv.memo {
                if have_l1 && (0..3).all(|i| (vox[i] >> 4) << 4 == l1_org[i]) {
                    level = 3;
                    sh = 2;
                    origin = l1_org;
                    mask = l1_mask;
                    base = l1_base;
                    stats.memo_entries += 1;
                } else if have_chunk && (0..3).all(|i| (vox[i] >> 6) << 6 == chunk_org[i]) {
                    level = 2;
                    sh = 4;
                    origin = chunk_org;
                    mask = chunk_mask;
                    base = chunk_base;
                    have_l1 = false;
                    stats.memo_entries += 1;
                } else {
                    have_chunk = false;
                    have_l1 = false;
                    stats.root_descents += 1;
                }
            } else {
                stats.root_descents += 1;
            }
            let mut cell: [i32; 3] = from_fn(|i| (vox[i] - origin[i]) >> sh);
            let descended = loop {
                let bit = cell_bit(cell, mir);
                if mask & (1u64 << bit) == 0 {
                    break true; // deepest empty level: DDA here
                }
                if level == 4 {
                    if t > max_t {
                        return hit;
                    }
                    hit.t = t * VOXEL_SIZE;
                    hit.vox = from_fn(|i| if mir[i] { -1 - vox[i] } else { vox[i] });
                    if lv.anyhit {
                        hit.mat = 1;
                    } else {
                        hit.n = from_fn(|i| -amask[i] * step_out[i]);
                        hit.mat = s.mats[(base + popcnt_below(mask, bit)) as usize];
                    }
                    return hit;
                }
                let corg: [i32; 3] = from_fn(|i| origin[i] + (cell[i] << sh));
                if level == 0 {
                    mask = s.tlas[2 + bit as usize * 2] as u64
                        | (s.tlas[3 + bit as usize * 2] as u64) << 32;
                } else if level == 1 {
                    let ccm: [i32; 3] = from_fn(|i| corg[i] >> 6);
                    let cc: [i32; 3] = from_fn(|i| if mir[i] { -1 - ccm[i] } else { ccm[i] });
                    let slot = s.tlas[130 + (cc[0] + cc[1] * 16 + cc[2] * 256) as usize];
                    let (m, b) = node4(s, slot as usize);
                    mask = m;
                    base = b;
                    if lv.memo {
                        have_chunk = true;
                        have_l1 = false;
                        chunk_org = corg;
                        chunk_mask = mask;
                        chunk_base = base;
                    }
                } else if level == 2 {
                    let (m, b) = node4(s, (base + popcnt_below(mask, bit)) as usize);
                    mask = m;
                    base = b;
                    if lv.memo {
                        have_l1 = true;
                        l1_org = corg;
                        l1_mask = mask;
                        l1_base = base;
                    }
                } else {
                    let (m, b) = leaf4(s, (base + popcnt_below(mask, bit)) as usize);
                    mask = m;
                    base = b;
                }
                level += 1;
                sh -= 2;
                origin = corg;
                cell = from_fn(|i| (vox[i] - origin[i]) >> sh);
            };
            assert!(descended);

            let cs = 1i32 << sh;
            let csf = cs as f32;
            let t_delta: [f32; 3] = from_fn(|i| inv_rd[i].abs() * csf);
            let mut t_max: [f32; 3] =
                from_fn(|i| (origin[i] as f32 + (cell[i] as f32 + step_ge[i]) * csf - ro[i]) * inv_rd[i]);
            loop {
                let mut jumped = false;
                if lv.skip {
                    let cu: [i32; 3] = from_fn(|i| if mir[i] { 3 - cell[i] } else { cell[i] });
                    let mw = (mask as u32, (mask >> 32) as u32);
                    if row_empty(mw, dom, cu) {
                        let sd = step_dir[dom];
                        if sd != 0 {
                            let n_steps = if sd > 0 { 4 - cell[dom] } else { cell[dom] + 1 };
                            let mut t_last = t_max[dom];
                            if n_steps > 1 {
                                t_last += t_delta[dom];
                            }
                            if n_steps > 2 {
                                t_last += t_delta[dom];
                            }
                            if n_steps > 3 {
                                t_last += t_delta[dom];
                            }
                            if axis_selected(dom, t_last, t_max) {
                                cell[dom] += sd * n_steps;
                                t = t_last;
                                t_max[dom] = t_last + t_delta[dom];
                                amask = [0.0; 3];
                                amask[dom] = 1.0;
                                jumped = true;
                                stats.jumps += 1;
                            }
                        }
                    }
                }
                if !jumped {
                    if t_max[0] <= t_max[1] && t_max[0] <= t_max[2] {
                        t = t_max[0];
                        t_max[0] += t_delta[0];
                        cell[0] += step_dir[0];
                        amask = [1.0, 0.0, 0.0];
                    } else if t_max[1] <= t_max[2] {
                        t = t_max[1];
                        t_max[1] += t_delta[1];
                        cell[1] += step_dir[1];
                        amask = [0.0, 1.0, 0.0];
                    } else {
                        t = t_max[2];
                        t_max[2] += t_delta[2];
                        cell[2] += step_dir[2];
                        amask = [0.0, 0.0, 1.0];
                    }
                }
                if t > t_exit {
                    return hit;
                }
                if cell.iter().all(|&c| (0..=3).contains(&c))
                    && mask & (1u64 << cell_bit(cell, mir)) == 0
                {
                    continue; // empty sibling: keep stepping here
                }
                let cell_base: [i32; 3] = from_fn(|i| origin[i] + (cell[i] << sh));
                let clamped: [i32; 3] = from_fn(|i| {
                    ((ro[i] + rdt[i] * t).floor() as i32).clamp(cell_base[i], cell_base[i] + cs - 1)
                });
                let crossing: [i32; 3] =
                    from_fn(|i| if step_pos[i] { cell_base[i] } else { cell_base[i] + cs - 1 });
                vox = from_fn(|i| if amask[i] > 0.5 { crossing[i] } else { clamped[i] });
                if (0..3).any(|i| vox[i] < glo[i] || vox[i] >= ghi[i]) {
                    return hit; // left the world grid
                }
                break; // outer loop re-descends at vox
            }
        }
        hit
    }

    // --- Independent oracle: f64 per-voxel Amanatides-Woo march over the
    // raw fixture voxels, same world clip / entry-nudge / tie-break
    // conventions as the kernels. Anchors the control port: a transcription
    // bug shared by control and opt ports would surface here.
    fn voxel_march(voxels: &[u32], ro_world: [f32; 3], rd: [f32; 3], max_t_world: f32) -> PortHit {
        let mut hit = PortHit { t: -1.0, n: from_fn(|i| -rd[i]), mat: 0, vox: [-1; 3] };
        let ro: [f64; 3] = from_fn(|i| ro_world[i] as f64 * 16.0);
        let rdd: [f64; 3] = from_fn(|i| rd[i] as f64);
        let max_t = max_t_world as f64 * 16.0;
        let inv: [f64; 3] = from_fn(|i| {
            let s = if rdd[i] >= 0.0 { 1.0f64 } else { -1.0 };
            s / rdd[i].abs().max(1e-12)
        });
        let t0: [f64; 3] = from_fn(|i| (0.0 - ro[i]) * inv[i]);
        let t1: [f64; 3] = from_fn(|i| (GRID_T as f64 - ro[i]) * inv[i]);
        let tmin3: [f64; 3] = from_fn(|i| t0[i].min(t1[i]));
        let tmax3: [f64; 3] = from_fn(|i| t0[i].max(t1[i]));
        let t_enter = tmin3[0].max(tmin3[1]).max(tmin3[2].max(0.0));
        let t_exit = tmax3[0].min(tmax3[1]).min(tmax3[2].min(max_t));
        if t_enter > t_exit {
            return hit;
        }
        let mut ax = if tmin3[0] >= tmin3[1] && tmin3[0] >= tmin3[2] {
            0usize
        } else if tmin3[1] >= tmin3[2] {
            1
        } else {
            2
        };
        let step: [i32; 3] = from_fn(|i| wgsl_sign(rd[i]));
        let step_ge: [f64; 3] = from_fn(|i| if rdd[i] >= 0.0 { 1.0 } else { 0.0 });
        let mut t = t_enter;
        let mut vox: [i32; 3] =
            from_fn(|i| ((ro[i] + rdd[i] * (t + 1e-4)).floor() as i32).clamp(0, GRID_T - 1));
        let mut t_max: [f64; 3] = from_fn(|i| (vox[i] as f64 + step_ge[i] - ro[i]) * inv[i]);
        let t_delta: [f64; 3] = from_fn(|i| inv[i].abs());
        for _ in 0..4096 {
            let m = fixture_voxel(voxels, vox[0], vox[1], vox[2]);
            if m != 0 {
                if t > max_t {
                    return hit;
                }
                hit.t = (t * 0.0625) as f32;
                hit.vox = vox;
                hit.mat = m;
                hit.n = [0.0; 3];
                hit.n[ax] = -(step[ax] as f32);
                return hit;
            }
            if t_max[0] <= t_max[1] && t_max[0] <= t_max[2] {
                ax = 0;
            } else if t_max[1] <= t_max[2] {
                ax = 1;
            } else {
                ax = 2;
            }
            t = t_max[ax];
            t_max[ax] += t_delta[ax];
            vox[ax] += step[ax];
            if t > t_exit || vox[ax] < 0 || vox[ax] >= GRID_T {
                return hit;
            }
        }
        panic!("voxel_march: step budget exceeded (ro {ro_world:?} rd {rd:?})");
    }

    // --- Deterministic ray bundle ------------------------------------------

    fn pcg(v: u32) -> u32 {
        let s = v.wrapping_mul(747796405).wrapping_add(2891336453);
        let w = ((s >> ((s >> 28) + 4)) ^ s).wrapping_mul(277803737);
        (w >> 22) ^ w
    }
    fn rnd01(state: &mut u32) -> f32 {
        *state = pcg(*state);
        (*state & 0xffffff) as f32 / 16777216.0
    }
    fn rnd_dir(state: &mut u32) -> [f32; 3] {
        let z = rnd01(state) * 2.0 - 1.0;
        let phi = rnd01(state) * std::f32::consts::TAU;
        let r = (1.0f32 - z * z).max(0.0).sqrt();
        [r * phi.cos(), r * phi.sin(), z]
    }

    /// (origin meters, unit direction, maxT meters). Fixture occupies the
    /// [0,16m)^3 corner of the 64 m world. >= 10k rays, fully deterministic.
    fn ray_bundle() -> Vec<([f32; 3], [f32; 3], f32)> {
        let mut rays = Vec::new();
        // Axis-aligned (exact-zero direction components), origins off any
        // voxel boundary, from outside the fixture region both ways.
        for axis in 0..3usize {
            for sgn in [1.0f32, -1.0] {
                for a in 0..24 {
                    for b in 0..24 {
                        let u = 0.37 + a as f32 * 0.653;
                        let v = 0.53 + b as f32 * 0.641;
                        let mut ro = [0.0f32; 3];
                        ro[axis] = if sgn > 0.0 { -1.3 } else { 17.3 };
                        ro[(axis + 1) % 3] = u;
                        ro[(axis + 2) % 3] = v;
                        let mut rd = [0.0f32; 3];
                        rd[axis] = sgn;
                        rays.push((ro, rd, 1e4));
                    }
                }
            }
        }
        // Exact diagonals through the interior (worst case for corner ties).
        let s3 = 1.0f32 / 3.0f32.sqrt();
        for d in 0..8u32 {
            let rd: [f32; 3] = from_fn(|i| if d >> i & 1 == 0 { s3 } else { -s3 });
            for a in 0..12 {
                for b in 0..12 {
                    let ro = [
                        1.13 + a as f32 * 1.171,
                        2.31 + b as f32 * 1.093,
                        3.77 + ((a * 7 + b * 3) % 12) as f32 * 1.031,
                    ];
                    rays.push((ro, rd, 1e4));
                }
            }
        }
        // Grazing rays: one dominant axis, one near-zero component.
        let mut st = 0x9e37u32;
        for axis in 0..3usize {
            for eps in [1e-4f32, 1e-3, 1e-2] {
                for k in 0..40 {
                    let mut rd = [0.0f32; 3];
                    rd[axis] = if k % 2 == 0 { 1.0 } else { -1.0 };
                    rd[(axis + 1) % 3] = if k % 4 < 2 { eps } else { -eps };
                    rd[(axis + 2) % 3] = 0.31 * (1.0 + 0.1 * (k % 5) as f32);
                    let l = (rd[0] * rd[0] + rd[1] * rd[1] + rd[2] * rd[2]).sqrt();
                    let rd: [f32; 3] = from_fn(|i| rd[i] / l);
                    let ro: [f32; 3] = from_fn(|_| rnd01(&mut st) * 18.0 - 1.0);
                    rays.push((ro, rd, 1e4));
                }
            }
        }
        // Random full-sphere rays, origins inside and around the fixture.
        let mut st = 12345u32;
        for _ in 0..6000 {
            let ro: [f32; 3] = from_fn(|_| rnd01(&mut st) * 20.0 - 2.0);
            rays.push((ro, rnd_dir(&mut st), 1e4));
        }
        // Short-maxT rays (exercise the t > maxT miss-at-solid path).
        let mut st = 777u32;
        for _ in 0..1500 {
            let ro: [f32; 3] = from_fn(|_| rnd01(&mut st) * 18.0 - 1.0);
            rays.push((ro, rnd_dir(&mut st), 0.05 + rnd01(&mut st) * 2.0));
        }
        assert!(rays.len() >= 10_000, "bundle has {} rays", rays.len());
        rays
    }

    // --- Comparators -------------------------------------------------------

    fn exact_eq(a: &PortHit, b: &PortHit, ignore_shading: bool) -> bool {
        if a.t.to_bits() != b.t.to_bits() {
            return false;
        }
        if a.t < 0.0 {
            return true;
        }
        a.vox == b.vox && (ignore_shading || (a.mat == b.mat && a.n == b.n))
    }

    /// Gate-v2-style comparison: |dt| <= max(1e-4 voxel, `ulps` ULP).
    fn t_close(a: f32, b: f32, ulps: f32) -> bool {
        let dt = (a - b).abs();
        if dt <= 6.25e-6 {
            return true;
        }
        let ulp = (f32::from_bits(a.to_bits() + 1) - a).max(f32::MIN_POSITIVE);
        dt / ulp <= ulps
    }

    fn consistent(a: &PortHit, b: &PortHit, ulps: f32) -> bool {
        if (a.t < 0.0) != (b.t < 0.0) {
            return false;
        }
        if a.t < 0.0 {
            return true;
        }
        a.mat == b.mat && a.vox == b.vox && t_close(a.t, b.t, ulps)
    }

    // --- Tests -------------------------------------------------------------

    /// Levers 1 (memo), 3 (skip) and any-hit are bitwise-exact vs the
    /// control by design: they must change only WHICH reads find the same
    /// cells (memo), or coalesce steps with identical serial arithmetic
    /// (skip), or skip shading-only bookkeeping (anyhit).
    #[test]
    fn opt_memo_skip_anyhit_bitwise_exact() {
        let (_voxels, s) = fixture_s64();
        let rays = ray_bundle();
        let configs: [(&str, Levers); 5] = [
            ("memo", Levers { memo: true, ..Default::default() }),
            ("skip", Levers { skip: true, ..Default::default() }),
            ("memo+skip", Levers { memo: true, skip: true, ..Default::default() }),
            ("anyhit", Levers { anyhit: true, ..Default::default() }),
            ("memo+skip+anyhit", Levers { memo: true, skip: true, anyhit: true, mirror: false }),
        ];
        let mut ctrl_stats = TraceStats::default();
        let control: Vec<PortHit> = rays
            .iter()
            .map(|&(ro, rd, mt)| trace_port(s, ro, rd, mt, Levers::default(), &mut ctrl_stats))
            .collect();
        let hits = control.iter().filter(|h| h.t >= 0.0).count();
        assert!(hits > rays.len() / 10, "bundle degenerate: only {hits} hits / {}", rays.len());

        for (name, lv) in configs {
            let mut stats = TraceStats::default();
            for (i, &(ro, rd, mt)) in rays.iter().enumerate() {
                let got = trace_port(s, ro, rd, mt, lv, &mut stats);
                assert!(
                    exact_eq(&control[i], &got, lv.anyhit),
                    "[{name}] ray {i} (ro {ro:?} rd {rd:?} maxT {mt}): control {:?} != opt {:?}",
                    control[i],
                    got
                );
            }
            // The lever must actually engage, not just pass vacuously.
            if lv.memo {
                assert!(stats.memo_entries > 0, "[{name}] memoized re-entry never taken");
                assert!(
                    stats.root_descents < ctrl_stats.root_descents,
                    "[{name}] memo did not reduce root descents ({} vs {})",
                    stats.root_descents,
                    ctrl_stats.root_descents
                );
            }
            if lv.skip {
                assert!(stats.jumps > 0, "[{name}] skip-coalescing jump never taken");
            }
        }
    }

    /// Lever 2 (mirroring) reflects through the origin, and IEEE negation is
    /// exact, so every t value is bitwise-identical to the control; the only
    /// representable divergence is a float position landing exactly on a
    /// cell boundary (floor(-w) != -floor(w) - 1 for integral w), a
    /// measure-zero knife-edge set. Held to the identity gate v2 standard
    /// (hit/miss + material + voxel agreement, |dt| <= max(1e-4 voxel,
    /// 16 ULP)) with a near-zero budget.
    #[test]
    fn opt_mirror_within_gate_tolerance() {
        let (_voxels, s) = fixture_s64();
        let rays = ray_bundle();
        let budget = rays.len() / 4000; // 0.025%
        for (name, lv) in [
            ("mirror", Levers { mirror: true, ..Default::default() }),
            ("memo+mirror+skip", Levers { memo: true, mirror: true, skip: true, anyhit: false }),
        ] {
            let mut divergent = 0usize;
            let mut st = TraceStats::default();
            for (i, &(ro, rd, mt)) in rays.iter().enumerate() {
                let a = trace_port(s, ro, rd, mt, Levers::default(), &mut st);
                let b = trace_port(s, ro, rd, mt, lv, &mut st);
                if !consistent(&a, &b, 16.0) {
                    divergent += 1;
                    if divergent <= 5 {
                        eprintln!("[{name}] divergent ray {i}: ro {ro:?} rd {rd:?}\n  control {a:?}\n  mirror  {b:?}");
                    }
                }
            }
            eprintln!("[{name}] divergent rays: {divergent} / {} (budget {budget})", rays.len());
            assert!(divergent <= budget, "[{name}] {divergent} divergent rays > budget {budget}");
        }
    }

    /// Anchor the control port itself against an independent f64 per-voxel
    /// march over the raw fixture array (catches transcription bugs shared
    /// by the control and opt ports). f32-vs-f64 knife edges are budgeted.
    #[test]
    fn control_port_matches_voxel_march_oracle() {
        let (voxels, s) = fixture_s64();
        let rays = ray_bundle();
        let budget = rays.len() / 500; // 0.2%
        let mut divergent = 0usize;
        let mut st = TraceStats::default();
        let mut hits = 0usize;
        for (i, &(ro, rd, mt)) in rays.iter().enumerate() {
            let a = voxel_march(voxels, ro, rd, mt);
            let b = trace_port(s, ro, rd, mt, Levers::default(), &mut st);
            hits += usize::from(b.t >= 0.0);
            if !consistent(&a, &b, 64.0) {
                divergent += 1;
                if divergent <= 5 {
                    eprintln!("divergent ray {i}: ro {ro:?} rd {rd:?}\n  march   {a:?}\n  control {b:?}");
                }
            }
        }
        eprintln!("oracle: {hits} hits, divergent {divergent} / {} (budget {budget})", rays.len());
        assert!(divergent <= budget, "{divergent} divergent rays > budget {budget}");
    }

    /// Helper properties: row-mask emptiness vs brute force over the 64-bit
    /// mask, mirrored bit-index round-trip, and the memo containment tests.
    #[test]
    fn opt_helper_properties() {
        // row_empty vs brute force.
        let mut st = 0xbeefu32;
        for _ in 0..4096 {
            let m = (pcg(st) as u64) << 32 | st as u64;
            st = pcg(st);
            let mw = (m as u32, (m >> 32) as u32);
            for axis in 0..3usize {
                for u in 0..4i32 {
                    for v in 0..4i32 {
                        let mut cu = [0i32; 3];
                        cu[(axis + 1) % 3] = u;
                        cu[(axis + 2) % 3] = v;
                        let brute = (0..4i32).all(|k| {
                            let mut c = cu;
                            c[axis] = k;
                            m >> (c[0] + c[1] * 4 + c[2] * 16) & 1 == 0
                        });
                        assert_eq!(
                            row_empty(mw, axis, cu),
                            brute,
                            "row_empty mask {m:#018x} axis {axis} cu {cu:?}"
                        );
                    }
                }
            }
        }
        // cell_bit: no-mirror identity, involution, and set preservation.
        for x in 0..4i32 {
            for y in 0..4i32 {
                for z in 0..4i32 {
                    let c = [x, y, z];
                    assert_eq!(cell_bit(c, [false; 3]), (x + y * 4 + z * 16) as u32);
                    for mbits in 0..8u32 {
                        let mir: [bool; 3] = from_fn(|i| mbits >> i & 1 == 1);
                        let cm: [i32; 3] = from_fn(|i| if mir[i] { 3 - c[i] } else { c[i] });
                        // mirroring the mirrored cell restores the bit
                        assert_eq!(cell_bit(cm, mir), (x + y * 4 + z * 16) as u32);
                    }
                }
            }
        }
        // Memo containment: (v >> k) << k == org  <=>  org <= v < org + 2^k
        // for 2^k-aligned org, including the mirrored negative coordinate
        // range (arithmetic shift floors toward -inf).
        let mut st = 4242u32;
        for _ in 0..20_000 {
            let v = (pcg(st) % 2048) as i32 - 1024;
            st = pcg(st);
            let org16 = ((pcg(st) % 128) as i32 - 64) * 16;
            st = pcg(st);
            let org64 = ((pcg(st) % 32) as i32 - 16) * 64;
            st = pcg(st);
            assert_eq!((v >> 4) << 4 == org16, org16 <= v && v < org16 + 16);
            assert_eq!((v >> 6) << 6 == org64, org64 <= v && v < org64 + 64);
        }
    }
}
