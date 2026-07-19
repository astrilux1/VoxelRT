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
}
