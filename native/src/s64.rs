// Host side of the sparse voxel 64-tree backend (docs/S64.md §3).
//
// Mirrors build_brickmap()'s shape: timestamped build passes, scans via
// scan.wgsl, structure byte accounting. See shaders/s64_build.wgsl for the
// full pass/dispatch structure; the phase split here exists because
// nodes/leaves/mats can only be allocated after the four allocation scans.

use std::borrow::Cow;

use crate::{buffer, compute_pipeline_with, gpu_scan, shader, Gpu, SceneSource, Timestamps};

const LGRID: u32 = 256; // leaf grid: 1024 / 4
const L1GRID: u32 = 64; // L1 grid: 1024 / 16
const CGRID: u32 = 16; // chunk grid: 1024 / 64
const NUM_LEAF_CELLS: u64 = (LGRID as u64).pow(3);
const NUM_L1_CELLS: u64 = (L1GRID as u64).pow(3);
const NUM_CHUNKS: u64 = (CGRID as u64).pow(3);
/// TLAS words: root mask (2) + 64 group masks (128) + chunkRoots (4096).
const TLAS_WORDS: u64 = 2 + 128 + NUM_CHUNKS;
/// Hard cap on total GPU allocation for this backend (docs/S64.md §4 keeps
/// the bench well under the 3080's VRAM; blow past this and something is
/// wrong with the build, so fail instead of thrashing).
const MAX_BYTES: u64 = 2 << 30;

pub struct S64Backend {
    pub tlas: wgpu::Buffer,
    pub nodes: wgpu::Buffer,
    pub leaves: wgpu::Buffer,
    pub mats: wgpu::Buffer,
    pub build_ms: Vec<f64>,
    pub root_count: u32,
    pub l1_count: u32,
    pub leaf_count: u32,
    pub mat_count: u32,
    pub bytes: u64,
}

fn bgl_entry(binding: u32, uniform: bool) -> wgpu::BindGroupLayoutEntry {
    wgpu::BindGroupLayoutEntry {
        binding,
        visibility: wgpu::ShaderStages::COMPUTE,
        ty: wgpu::BindingType::Buffer {
            ty: if uniform {
                wgpu::BufferBindingType::Uniform
            } else {
                wgpu::BufferBindingType::Storage { read_only: false }
            },
            has_dynamic_offset: false,
            min_binding_size: None,
        },
        count: None,
    }
}

/// Bind group layout for bindings 0..n of s64_build.wgsl (0 = uniform).
/// Phase A uses bindings 0..=8; phase B additionally needs nodes/leaves/mats
/// (9..=11), which do not exist until after the scans — hence two layouts.
fn build_layout(gpu: &Gpu, n: u32) -> wgpu::BindGroupLayout {
    let entries: Vec<wgpu::BindGroupLayoutEntry> = (0..n).map(|i| bgl_entry(i, i == 0)).collect();
    gpu.device
        .create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some("s64-build"), entries: &entries })
}

fn bind_all(gpu: &Gpu, layout: &wgpu::BindGroupLayout, buffers: &[&wgpu::Buffer]) -> wgpu::BindGroup {
    let entries: Vec<wgpu::BindGroupEntry> = buffers
        .iter()
        .enumerate()
        .map(|(i, b)| wgpu::BindGroupEntry { binding: i as u32, resource: b.as_entire_binding() })
        .collect();
    gpu.device
        .create_bind_group(&wgpu::BindGroupDescriptor { label: Some("s64-build"), layout, entries: &entries })
}

pub fn build_s64(gpu: &Gpu, shader_dir: &str, seed: u32, scene: &SceneSource) -> S64Backend {
    let src = shader(shader_dir, &["gen.wgsl", "source.wgsl", "s64_build.wgsl"]);
    let module = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("s64-build"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(src)),
    });
    let scan_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("scan"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(shader(shader_dir, &["scan.wgsl"]))),
    });

    let params = buffer(&gpu.device, "s64-params", 16, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
    gpu.queue.write_buffer(&params, 0, bytemuck::cast_slice(&[seed, 0u32, scene.mode, 0u32]));

    // srcVoxels (source.wgsl @group(1)): shared by every build pipeline.
    let src_layout = gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
        label: Some("s64-src"),
        entries: &[wgpu::BindGroupLayoutEntry {
            binding: 0,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: wgpu::BufferBindingType::Storage { read_only: true },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        }],
    });
    let bg_src = bind_all(gpu, &src_layout, &[&scene.buf]);

    // Build intermediates (~72 MB total; the 256^3 count array dominates).
    let st = wgpu::BufferUsages::STORAGE;
    let st_scan = st | wgpu::BufferUsages::COPY_SRC; // gpu_scan reads back the last element
    let leaf_count = buffer(&gpu.device, "s64-leaf-count", NUM_LEAF_CELLS * 4, st);
    let l1_mask = buffer(&gpu.device, "s64-l1-mask", NUM_L1_CELLS * 8, st);
    let l1_child = buffer(&gpu.device, "s64-l1-child", NUM_L1_CELLS * 4, st_scan);
    let l1_mat = buffer(&gpu.device, "s64-l1-mat", NUM_L1_CELLS * 4, st_scan);
    let chunk_mask = buffer(&gpu.device, "s64-chunk-mask", NUM_CHUNKS * 8, st);
    let root_child = buffer(&gpu.device, "s64-root-child", NUM_CHUNKS * 4, st_scan);
    let chunk_occ = buffer(&gpu.device, "s64-chunk-occ", NUM_CHUNKS * 4, st_scan);
    let tlas = buffer(&gpu.device, "s64-tlas", TLAS_WORDS * 4, st | wgpu::BufferUsages::COPY_SRC);
    let intermediate_bytes = NUM_LEAF_CELLS * 4 + NUM_L1_CELLS * 16 + NUM_CHUNKS * 16;

    let ts = Timestamps::new(gpu, 8);

    // --- phase A: counts + occupancy reduction leaf -> L1 -> root -> TLAS ---
    let layout_a = build_layout(gpu, 9);
    let pl_a = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("s64-build-a"),
        bind_group_layouts: &[Some(&layout_a), Some(&src_layout)],
        ..Default::default()
    });
    let p_leaf_count = compute_pipeline_with(gpu, &module, "pass_leaf_count", Some(&pl_a));
    let p_l1 = compute_pipeline_with(gpu, &module, "pass_l1", Some(&pl_a));
    let p_chunk = compute_pipeline_with(gpu, &module, "pass_chunk", Some(&pl_a));
    let p_tlas = compute_pipeline_with(gpu, &module, "pass_tlas", Some(&pl_a));
    let bufs_a = [&params, &leaf_count, &l1_mask, &l1_child, &l1_mat, &chunk_mask, &root_child, &chunk_occ, &tlas];
    let bg_a = bind_all(gpu, &layout_a, &bufs_a);

    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("s64-leaf-count"),
            timestamp_writes: Some(ts.pass_writes(0)),
        });
        pass.set_pipeline(&p_leaf_count);
        pass.set_bind_group(0, &bg_a, &[]);
        pass.set_bind_group(1, &bg_src, &[]);
        pass.dispatch_workgroups(LGRID, LGRID, LGRID);
    }
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("s64-reduce"),
            timestamp_writes: Some(ts.pass_writes(2)),
        });
        pass.set_bind_group(0, &bg_a, &[]);
        pass.set_bind_group(1, &bg_src, &[]);
        pass.set_pipeline(&p_l1);
        pass.dispatch_workgroups(L1GRID, L1GRID, L1GRID);
        pass.set_pipeline(&p_chunk);
        pass.dispatch_workgroups(CGRID, CGRID, CGRID);
        pass.set_pipeline(&p_tlas);
        pass.dispatch_workgroups(1, 1, 1);
    }
    gpu.queue.submit([enc.finish()]);

    // --- allocation scans (untimed, matching build_brickmap's scan) --------
    let leaf_total = gpu_scan(gpu, &scan_mod, &l1_child, NUM_L1_CELLS as u32);
    let mat_total = gpu_scan(gpu, &scan_mod, &l1_mat, NUM_L1_CELLS as u32);
    let l1_total = gpu_scan(gpu, &scan_mod, &root_child, NUM_CHUNKS as u32);
    let root_total = gpu_scan(gpu, &scan_mod, &chunk_occ, NUM_CHUNKS as u32);
    assert!(root_total > 0 && leaf_total > 0 && mat_total > 0, "scene generated an empty S64 structure");

    let node_bytes = (root_total as u64 + l1_total as u64) * 16;
    let leaf_bytes = leaf_total as u64 * 16;
    let mat_bytes = mat_total as u64 * 4;
    let bytes = TLAS_WORDS * 4 + node_bytes + leaf_bytes + mat_bytes;
    if bytes + intermediate_bytes > MAX_BYTES {
        eprintln!(
            "s64 build would allocate {:.2} GB (> {:.0} GB cap): roots {root_total}, l1 {l1_total}, leaves {leaf_total}, mats {mat_total}",
            (bytes + intermediate_bytes) as f64 / 1e9,
            MAX_BYTES as f64 / 1e9,
        );
        std::process::exit(1);
    }

    let nodes = buffer(&gpu.device, "s64-nodes", node_bytes, st | wgpu::BufferUsages::COPY_SRC);
    let leaves = buffer(&gpu.device, "s64-leaves", leaf_bytes, st | wgpu::BufferUsages::COPY_SRC);
    let mats = buffer(&gpu.device, "s64-mats", mat_bytes, st | wgpu::BufferUsages::COPY_SRC);

    // --- phase B: pointer/compaction writes (numRoots now known) -----------
    gpu.queue.write_buffer(&params, 0, bytemuck::cast_slice(&[seed, root_total, scene.mode, 0u32]));
    let layout_b = build_layout(gpu, 12);
    let pl_b = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("s64-build-b"),
        bind_group_layouts: &[Some(&layout_b), Some(&src_layout)],
        ..Default::default()
    });
    let p_matbase = compute_pipeline_with(gpu, &module, "pass_matbase", Some(&pl_b));
    let p_roots = compute_pipeline_with(gpu, &module, "pass_roots", Some(&pl_b));
    let p_l1_write = compute_pipeline_with(gpu, &module, "pass_l1_write", Some(&pl_b));
    let p_leaf_fill = compute_pipeline_with(gpu, &module, "pass_leaf_fill", Some(&pl_b));
    let bufs_b = [
        &params, &leaf_count, &l1_mask, &l1_child, &l1_mat, &chunk_mask, &root_child, &chunk_occ, &tlas, &nodes,
        &leaves, &mats,
    ];
    let bg_b = bind_all(gpu, &layout_b, &bufs_b);

    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("s64-pointers"),
            timestamp_writes: Some(ts.pass_writes(4)),
        });
        pass.set_bind_group(0, &bg_b, &[]);
        pass.set_bind_group(1, &bg_src, &[]);
        pass.set_pipeline(&p_matbase);
        pass.dispatch_workgroups(L1GRID, L1GRID, L1GRID);
        pass.set_pipeline(&p_roots);
        pass.dispatch_workgroups(NUM_CHUNKS as u32 / 64, 1, 1);
        pass.set_pipeline(&p_l1_write);
        pass.dispatch_workgroups(NUM_L1_CELLS as u32 / 64, 1, 1);
    }
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("s64-leaf-fill"),
            timestamp_writes: Some(ts.pass_writes(6)),
        });
        pass.set_pipeline(&p_leaf_fill);
        pass.set_bind_group(0, &bg_b, &[]);
        pass.set_bind_group(1, &bg_src, &[]);
        pass.dispatch_workgroups(LGRID, LGRID, LGRID);
    }
    gpu.queue.submit([enc.finish()]);
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
    let build_ms = ts.read_ms(gpu, 4); // [leafCount, reduce, pointers, leafFill]

    S64Backend {
        tlas,
        nodes,
        leaves,
        mats,
        build_ms,
        root_count: root_total,
        l1_count: l1_total,
        leaf_count: leaf_total,
        mat_count: mat_total,
        bytes,
    }
}
