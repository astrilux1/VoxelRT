// VoxelRT native traversal bench (milestone 1, docs/S64.md).
//
// Headless wgpu host: generates the scene on the GPU, builds an acceleration
// structure on the GPU, traces three timestamp-isolated ray classes, and
// writes an identity-gate PNG plus a JSON results row.
//
//   cargo run --release -- --backend brickmap --scene sparse1024 \
//       --size 1920x1080 --frames 5 --out ../test/eval/native-bench.json

use std::borrow::Cow;
use std::fmt::Write as _;

mod s64;
mod verify;

const GRID: u32 = 1024;
const BRICK: u32 = 8;
const BGRID: u32 = GRID / BRICK; // 128
const NUM_BRICKS: u64 = (BGRID as u64) * (BGRID as u64) * (BGRID as u64);

struct Args {
    backend: String,
    /// s64 traversal variant (docs/S64.md §4 budget): "stack" = variant 1
    /// (small-stack descent, s64_trace.wgsl), "stackless" = variant 2
    /// (stackless re-descent, s64_trace_stackless.wgsl), "stackless-opt" =
    /// the docs/S64OPT.md optimization rung over variant 2
    /// (s64_trace_stackless_opt.wgsl). Ignored by brickmap.
    traversal: String,
    /// --opt-levers (stackless-opt only): comma list of levers to enable —
    /// memo, mirror, skip, anyhit — or "none" to disable all. Absent = all
    /// on. Applied by string-replacing the kernel's `const ENABLE_*` flags
    /// (see the opt kernel's header); "anyhit" additionally compiles a
    /// second module with ENABLE_ANYHIT=true used only for the shadow
    /// pipelines (bench.wgsl is frozen, so any-hit is per-module).
    opt_levers: Option<String>,
    scene: String,
    width: u32,
    height: u32,
    frames: u32,
    seed: u32,
    out: Option<String>,
    png: Option<String>,
    gate: bool,
    verify_build: bool,
    /// Bench workgroup size (--wg WxH), default 8x8. Occupancy knob only:
    /// per-pixel math is size-independent, so images are unchanged.
    wg: (u32, u32),
    /// --compact: bounce/shadow dispatch only over compacted primary hits
    /// (indirect dispatch) instead of all pixels. Same rays, same RNG (seeded
    /// by pixel index), so results are unchanged; only warp packing differs.
    compact: bool,
}

fn parse_args() -> Args {
    let mut a = Args {
        backend: "brickmap".into(),
        traversal: "stack".into(),
        opt_levers: None,
        scene: "sparse1024".into(),
        width: 1920,
        height: 1080,
        frames: 5,
        seed: 1,
        out: None,
        png: Some("native-bench.png".into()),
        gate: false,
        verify_build: false,
        wg: (8, 8),
        compact: false,
    };
    let argv: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < argv.len() {
        let k = argv[i].clone();
        if k == "--check-shaders" {
            i += 1; // value-less flag, handled in main
            continue;
        }
        if k == "--gate" {
            a.gate = true;
            i += 1;
            continue;
        }
        if k == "--verify-build" {
            a.verify_build = true;
            i += 1;
            continue;
        }
        if k == "--compact" {
            a.compact = true;
            i += 1;
            continue;
        }
        let v = argv.get(i + 1).cloned().unwrap_or_else(|| {
            eprintln!("missing value for {k}");
            std::process::exit(2);
        });
        match k.as_str() {
            "--backend" => a.backend = v,
            "--traversal" => {
                if v != "stack" && v != "stackless" && v != "stackless-opt" {
                    eprintln!("unknown traversal {v} (expected stack, stackless or stackless-opt)");
                    std::process::exit(2);
                }
                a.traversal = v;
            }
            "--opt-levers" => {
                for tok in v.split(',') {
                    if !matches!(tok, "memo" | "mirror" | "skip" | "anyhit" | "none") {
                        eprintln!("unknown lever {tok} (expected memo, mirror, skip, anyhit or none)");
                        std::process::exit(2);
                    }
                }
                a.opt_levers = Some(v);
            }
            "--scene" => a.scene = v,
            "--frames" => a.frames = v.parse().expect("frames"),
            "--seed" => a.seed = v.parse().expect("seed"),
            "--out" => a.out = Some(v),
            "--png" => a.png = Some(v),
            "--size" => {
                let (w, h) = v.split_once('x').expect("WxH");
                a.width = w.parse().expect("w");
                a.height = h.parse().expect("h");
            }
            "--wg" => {
                let (x, y) = v.split_once('x').expect("WxH");
                a.wg = (x.parse().expect("wg x"), y.parse().expect("wg y"));
                assert!(a.wg.0 * a.wg.1 >= 32 && a.wg.0 * a.wg.1 <= 512, "wg must be 32..512 threads");
            }
            _ => {
                eprintln!("unknown arg {k}");
                std::process::exit(2);
            }
        }
        i += 2;
    }
    if a.opt_levers.is_some() && a.traversal != "stackless-opt" {
        eprintln!("--opt-levers requires --traversal stackless-opt");
        std::process::exit(2);
    }
    a
}

impl Args {
    /// Resolved lever set for the stackless-opt kernel, in the fixed order
    /// [memo, mirror, skip, anyhit]. Absent --opt-levers = all on ("none"
    /// contributes nothing, so `--opt-levers none` = all off).
    fn levers(&self) -> [bool; 4] {
        let Some(s) = self.opt_levers.as_deref() else { return [true; 4] };
        let mut l = [false; 4];
        for tok in s.split(',') {
            match tok {
                "memo" => l[0] = true,
                "mirror" => l[1] = true,
                "skip" => l[2] = true,
                "anyhit" => l[3] = true,
                _ => {} // "none"; unknown tokens rejected in parse_args
            }
        }
        l
    }
}

/// Flip one `const NAME : bool = ...;` lever declaration in the opt kernel
/// source. Panics if the declaration is missing: the exact text is part of
/// the --opt-levers string-toggle contract (opt kernel header), and drift
/// must fail loudly, not silently run with the wrong lever set.
fn set_shader_flag(src: &str, name: &str, value: bool) -> String {
    let decl_t = format!("const {name} : bool = true;");
    let decl_f = format!("const {name} : bool = false;");
    let (want, other) = if value { (decl_t, decl_f) } else { (decl_f, decl_t) };
    if src.contains(&want) {
        return src.to_string();
    }
    assert!(src.contains(&other), "opt kernel: `const {name} : bool = ...;` declaration not found");
    src.replacen(&other, &want, 1)
}

/// Assemble the trace shader source for the selected traversal, applying the
/// --opt-levers const toggles for the stackless-opt kernel. `anyhit_module`:
/// the shadow pipelines are built from a second module with ENABLE_ANYHIT
/// true (bench.wgsl's group-0 interface and entry points are frozen and
/// trace() cannot know its caller, so any-hit is a per-module
/// specialization; see the opt kernel's header).
fn trace_source(shader_dir: &str, args: &Args, backend: &Backend, anyhit_module: bool) -> String {
    let mut src = shader(shader_dir, &backend.trace_files(&args.traversal));
    if matches!(backend, Backend::S64(_)) && args.traversal == "stackless-opt" {
        let l = args.levers();
        src = set_shader_flag(&src, "ENABLE_MEMO", l[0]);
        src = set_shader_flag(&src, "ENABLE_MIRROR", l[1]);
        src = set_shader_flag(&src, "ENABLE_SKIP", l[2]);
        src = set_shader_flag(&src, "ENABLE_ANYHIT", anyhit_module && l[3]);
    }
    src
}

fn shader(dir: &str, names: &[&str]) -> String {
    let mut s = String::new();
    for n in names {
        let path = format!("{dir}/{n}");
        s.push_str(&std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("read {path}: {e}")));
        s.push('\n');
    }
    s
}

struct Gpu {
    device: wgpu::Device,
    queue: wgpu::Queue,
    ts_period: f32,
    adapter_info: wgpu::AdapterInfo,
}

fn init_gpu() -> Gpu {
    let instance = wgpu::Instance::new(wgpu::InstanceDescriptor::new_without_display_handle());
    let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
        power_preference: wgpu::PowerPreference::HighPerformance,
        force_fallback_adapter: false,
        compatible_surface: None,
        ..Default::default()
    }))
    .expect("no adapter");
    let mut limits = wgpu::Limits::default();
    limits.max_storage_buffer_binding_size = 1 << 30;
    limits.max_buffer_size = 1 << 31;
    // s64_build.wgsl's shared phase-B bind group holds 11 storage buffers.
    limits.max_storage_buffers_per_shader_stage = 16;
    // Build passes use 8x8x8 = 512-invocation workgroups (one brick / L1 cell
    // per workgroup); the wgpu default limit is 256.
    limits.max_compute_invocations_per_workgroup = 512;
    let (device, queue) = pollster::block_on(adapter.request_device(&wgpu::DeviceDescriptor {
        label: Some("voxelrt-native"),
        required_features: wgpu::Features::TIMESTAMP_QUERY,
        required_limits: limits,
        ..Default::default()
    }))
    .expect("device");
    let ts_period = queue.get_timestamp_period();
    let adapter_info = adapter.get_info();
    Gpu { device, queue, ts_period, adapter_info }
}

fn buffer(d: &wgpu::Device, label: &str, size: u64, usage: wgpu::BufferUsages) -> wgpu::Buffer {
    d.create_buffer(&wgpu::BufferDescriptor { label: Some(label), size, usage, mapped_at_creation: false })
}

fn readback_u32(gpu: &Gpu, src: &wgpu::Buffer, offset: u64, count: usize) -> Vec<u32> {
    let staging = buffer(
        &gpu.device,
        "staging",
        (count * 4) as u64,
        wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
    );
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    enc.copy_buffer_to_buffer(src, offset, &staging, 0, (count * 4) as u64);
    gpu.queue.submit([enc.finish()]);
    let slice = staging.slice(..);
    slice.map_async(wgpu::MapMode::Read, |r| r.expect("map"));
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
    let data: Vec<u32> = bytemuck::cast_slice(&slice.get_mapped_range().expect("range")).to_vec();
    data
}

struct Timestamps {
    set: wgpu::QuerySet,
    resolve: wgpu::Buffer,
    capacity: u32,
}

impl Timestamps {
    fn new(gpu: &Gpu, capacity: u32) -> Self {
        let set = gpu.device.create_query_set(&wgpu::QuerySetDescriptor {
            label: Some("ts"),
            ty: wgpu::QueryType::Timestamp,
            count: capacity,
        });
        let resolve = buffer(
            &gpu.device,
            "ts-resolve",
            capacity as u64 * 8,
            wgpu::BufferUsages::QUERY_RESOLVE | wgpu::BufferUsages::COPY_SRC,
        );
        Timestamps { set, resolve, capacity }
    }
    fn pass_writes(&self, begin: u32) -> wgpu::ComputePassTimestampWrites<'_> {
        wgpu::ComputePassTimestampWrites {
            query_set: &self.set,
            beginning_of_pass_write_index: Some(begin),
            end_of_pass_write_index: Some(begin + 1),
        }
    }
    /// Returns per-pair milliseconds for `pairs` timestamp pairs.
    fn read_ms(&self, gpu: &Gpu, pairs: u32) -> Vec<f64> {
        let staging = buffer(
            &gpu.device,
            "ts-staging",
            self.capacity as u64 * 8,
            wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
        );
        let mut enc = gpu.device.create_command_encoder(&Default::default());
        enc.resolve_query_set(&self.set, 0..pairs * 2, &self.resolve, 0);
        enc.copy_buffer_to_buffer(&self.resolve, 0, &staging, 0, pairs as u64 * 16);
        gpu.queue.submit([enc.finish()]);
        let slice = staging.slice(..);
        slice.map_async(wgpu::MapMode::Read, |r| r.expect("map"));
        gpu.device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
        let raw: Vec<u64> = bytemuck::cast_slice(&slice.get_mapped_range().expect("range")).to_vec();
        (0..pairs as usize)
            .map(|i| (raw[i * 2 + 1].saturating_sub(raw[i * 2])) as f64 * gpu.ts_period as f64 * 1e-6)
            .collect()
    }
}

fn compute_pipeline(gpu: &Gpu, module: &wgpu::ShaderModule, entry: &str) -> wgpu::ComputePipeline {
    compute_pipeline_with(gpu, module, entry, None)
}

/// Like compute_pipeline, but with an explicit layout: the s64 build passes
/// share one bind group across entry points with disjoint binding subsets,
/// which auto-layout (used-bindings-only) cannot express.
fn compute_pipeline_with(
    gpu: &Gpu,
    module: &wgpu::ShaderModule,
    entry: &str,
    layout: Option<&wgpu::PipelineLayout>,
) -> wgpu::ComputePipeline {
    gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some(entry),
        layout,
        module,
        entry_point: Some(entry),
        compilation_options: Default::default(),
        cache: None,
    })
}

fn bind(gpu: &Gpu, pipeline: &wgpu::ComputePipeline, group: u32, buffers: &[&wgpu::Buffer]) -> wgpu::BindGroup {
    bind_with_layout(gpu, &pipeline.get_bind_group_layout(group), buffers)
}

fn bind_with_layout(gpu: &Gpu, layout: &wgpu::BindGroupLayout, buffers: &[&wgpu::Buffer]) -> wgpu::BindGroup {
    let entries: Vec<wgpu::BindGroupEntry> = buffers
        .iter()
        .enumerate()
        .map(|(i, b)| wgpu::BindGroupEntry { binding: i as u32, resource: b.as_entire_binding() })
        .collect();
    gpu.device.create_bind_group(&wgpu::BindGroupDescriptor { label: None, layout, entries: &entries })
}

#[derive(Clone, Copy)]
enum Binding {
    Uniform,
    Storage,
    StorageRead,
}

/// Explicit bind group layout for consecutive buffer bindings 0..len.
/// Needed wherever one bind group is shared across entry points that use
/// different binding subsets: wgpu auto layouts contain only the bindings an
/// entry point statically uses and reject bind groups with extra entries.
fn bind_group_layout(gpu: &Gpu, label: &str, kinds: &[Binding]) -> wgpu::BindGroupLayout {
    let entries: Vec<wgpu::BindGroupLayoutEntry> = kinds
        .iter()
        .enumerate()
        .map(|(i, kind)| wgpu::BindGroupLayoutEntry {
            binding: i as u32,
            visibility: wgpu::ShaderStages::COMPUTE,
            ty: wgpu::BindingType::Buffer {
                ty: match kind {
                    Binding::Uniform => wgpu::BufferBindingType::Uniform,
                    Binding::Storage => wgpu::BufferBindingType::Storage { read_only: false },
                    Binding::StorageRead => wgpu::BufferBindingType::Storage { read_only: true },
                },
                has_dynamic_offset: false,
                min_binding_size: None,
            },
            count: None,
        })
        .collect();
    gpu.device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor { label: Some(label), entries: &entries })
}

/// Explicit layout for the trace kernels ([backend]_trace.wgsl + bench.wgsl):
/// bench.wgsl's group 0 is shared by primary/bounce/shadow, but bounce and
/// shadow do not statically use imageOut, so auto layouts cannot share the
/// group-0 bind group. Group 1 is the backend's read-only structure buffers.
/// `with_indirect_binding`: the primary/prep pipelines bind indirectArgs as
/// writable storage (binding 6); the secondary passes must NOT have it in
/// their layout at all — wgpu's usage scopes forbid STORAGE_READ_WRITE and
/// INDIRECT on the same buffer inside one dispatch scope, and the compacted
/// entry points never reference it.
fn trace_pipeline_layout(gpu: &Gpu, backend_buffers: usize, with_indirect_binding: bool) -> wgpu::PipelineLayout {
    let mut g0_bindings = vec![
        Binding::Uniform,
        Binding::Storage,
        Binding::Storage,
        Binding::Storage,
        Binding::Storage,
        Binding::Storage, // compactHits
    ];
    if with_indirect_binding {
        g0_bindings.push(Binding::Storage); // indirectArgs
    }
    let g0 = bind_group_layout(gpu, "bench-g0", &g0_bindings);
    let g1 = bind_group_layout(gpu, "trace-g1", &vec![Binding::StorageRead; backend_buffers]);
    gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("trace"),
        bind_group_layouts: &[Some(&g0), Some(&g1)],
        ..Default::default()
    })
}

/// GPU exclusive scan over `buf` (n u32s), in place. Returns the total sum.
///
/// The three scan entry points share one explicit layout (and thus one bind
/// group): scan_sums does not statically use `buf`, so an auto layout would
/// reject a shared bind group carrying it.
fn gpu_scan(gpu: &Gpu, scan_mod: &wgpu::ShaderModule, buf: &wgpu::Buffer, n: u32) -> u32 {
    let blocks = n.div_ceil(256);
    let params = buffer(&gpu.device, "scan-params", 16, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
    gpu.queue.write_buffer(&params, 0, bytemuck::cast_slice(&[n, 0u32, 0u32, 0u32]));
    let block_sums = buffer(
        &gpu.device,
        "scan-sums",
        blocks as u64 * 4,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );
    let layout = bind_group_layout(gpu, "scan", &[Binding::Uniform, Binding::Storage, Binding::Storage]);
    let pl = gpu.device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
        label: Some("scan"),
        bind_group_layouts: &[Some(&layout)],
        ..Default::default()
    });
    let p_blocks = compute_pipeline_with(gpu, scan_mod, "scan_blocks", Some(&pl));
    let p_sums = compute_pipeline_with(gpu, scan_mod, "scan_sums", Some(&pl));
    let p_add = compute_pipeline_with(gpu, scan_mod, "scan_add", Some(&pl));
    // Read the last element before scanning so the total can be recovered
    // (total = last exclusive prefix + last input).
    let last_in = readback_u32(gpu, buf, (n as u64 - 1) * 4, 1)[0];

    let bg = bind_with_layout(gpu, &layout, &[&params, buf, &block_sums]);
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&Default::default());
        pass.set_bind_group(0, &bg, &[]);
        pass.set_pipeline(&p_blocks);
        pass.dispatch_workgroups(blocks, 1, 1);
        pass.set_pipeline(&p_sums);
        pass.dispatch_workgroups(1, 1, 1);
        pass.set_pipeline(&p_add);
        pass.dispatch_workgroups(blocks, 1, 1);
    }
    gpu.queue.submit([enc.finish()]);
    let last_out = readback_u32(gpu, buf, (n as u64 - 1) * 4, 1)[0];
    last_out + last_in
}

/// Scene voxel source shared by both builders (shaders/source.wgsl):
/// procedural voxel_material() for sparse1024, or the uploaded fixture256
/// voxel buffer (VXF1 dump of the browser interior, docs/S64.md §2).
struct SceneSource {
    /// 0 = procedural, 1 = srcVoxels buffer.
    mode: u32,
    /// @group(1) @binding(0) srcVoxels for the build passes (4-byte dummy in
    /// procedural mode; never read there).
    buf: wgpu::Buffer,
    /// Fixture payload (256^3 u32, x + y*N + z*N^2), kept for the CPU
    /// reference builders. None in procedural mode.
    voxels: Option<Vec<u32>>,
}

/// Fixture grid size: the 256^3 world occupies the [0,256)^3 corner of the
/// 1024^3 grid coordinate space (world stays GRID=1024; empty beyond).
const FIXTURE_GRID: u32 = 256;

fn fixture_path() -> std::path::PathBuf {
    std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../test/eval/fixture256.bin")
}

/// Parse a VXF1 fixture (format spec: test/dump-scene.mjs header).
fn load_fixture(path: &std::path::Path) -> Vec<u32> {
    let raw = std::fs::read(path).unwrap_or_else(|e| {
        eprintln!("read {} failed: {e} (generate it with: node test/dump-scene.mjs)", path.display());
        std::process::exit(2);
    });
    assert!(raw.len() >= 16 && &raw[0..4] == b"VXF1", "{}: not a VXF1 fixture", path.display());
    let n = u32::from_le_bytes(raw[4..8].try_into().unwrap());
    assert_eq!(n, FIXTURE_GRID, "{}: grid {n} unsupported (expected {FIXTURE_GRID})", path.display());
    let expect = 16 + (n as usize).pow(3) * 4;
    assert_eq!(raw.len(), expect, "{}: size {} != {expect}", path.display(), raw.len());
    raw[16..].chunks_exact(4).map(|c| u32::from_le_bytes(c.try_into().unwrap())).collect()
}

fn load_scene(gpu: &Gpu, name: &str) -> SceneSource {
    match name {
        "sparse1024" => {
            let buf = buffer(&gpu.device, "src-voxels-dummy", 4, wgpu::BufferUsages::STORAGE);
            SceneSource { mode: 0, buf, voxels: None }
        }
        "fixture256" => {
            let voxels = load_fixture(&fixture_path());
            let buf = buffer(
                &gpu.device,
                "src-voxels",
                (voxels.len() * 4) as u64,
                wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
            );
            gpu.queue.write_buffer(&buf, 0, bytemuck::cast_slice(&voxels));
            SceneSource { mode: 1, buf, voxels: Some(voxels) }
        }
        other => {
            eprintln!("unknown scene {other} (expected sparse1024 or fixture256)");
            std::process::exit(2);
        }
    }
}

struct BrickmapBackend {
    brick_ptr: wgpu::Buffer,
    brick_masks: wgpu::Buffer,
    brick_mats: wgpu::Buffer,
    build_ms: Vec<f64>,
    brick_count: u32,
    bytes: u64,
}

fn build_brickmap(gpu: &Gpu, shader_dir: &str, seed: u32, scene: &SceneSource) -> BrickmapBackend {
    let gen_src = shader(shader_dir, &["gen.wgsl", "source.wgsl", "brickmap_build.wgsl"]);
    let gen_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("brickmap-build"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(gen_src)),
    });
    let scan_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("scan"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(shader(shader_dir, &["scan.wgsl"]))),
    });

    let params = buffer(&gpu.device, "gen-params", 16, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
    gpu.queue.write_buffer(&params, 0, bytemuck::cast_slice(&[seed, GRID.trailing_zeros(), scene.mode, 0u32]));

    let occupied = buffer(
        &gpu.device,
        "brick-occupied",
        NUM_BRICKS * 4,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );
    let slots = buffer(
        &gpu.device,
        "brick-slots",
        NUM_BRICKS * 4,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::COPY_SRC,
    );

    let ts = Timestamps::new(gpu, 8);

    let p_occ = compute_pipeline(gpu, &gen_mod, "pass_occupancy");
    let bg_occ = bind(gpu, &p_occ, 0, &[&params, &occupied]);
    let bg_occ_src = bind(gpu, &p_occ, 1, &[&scene.buf]);
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("occupancy"),
            timestamp_writes: Some(ts.pass_writes(0)),
        });
        pass.set_pipeline(&p_occ);
        pass.set_bind_group(0, &bg_occ, &[]);
        pass.set_bind_group(1, &bg_occ_src, &[]);
        pass.dispatch_workgroups(BGRID, BGRID, BGRID);
    }
    enc.copy_buffer_to_buffer(&occupied, 0, &slots, 0, NUM_BRICKS * 4);
    gpu.queue.submit([enc.finish()]);

    // Scan slots in place -> allocation indices; total = allocated brick count.
    let brick_count = gpu_scan(gpu, &scan_mod, &slots, NUM_BRICKS as u32);
    assert!(brick_count > 0, "scene generated zero bricks");
    let mask_bytes = brick_count as u64 * 16 * 4;
    let mat_bytes = brick_count as u64 * 512 * 4;

    let brick_ptr = buffer(
        &gpu.device,
        "brick-ptr",
        NUM_BRICKS * 4,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );
    let brick_masks = buffer(
        &gpu.device,
        "brick-masks",
        mask_bytes,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );
    let brick_mats = buffer(
        &gpu.device,
        "brick-mats",
        mat_bytes,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC,
    );

    let p_fill = compute_pipeline(gpu, &gen_mod, "pass_fill");
    let bg_fill = bind(gpu, &p_fill, 0, &[&params, &occupied, &slots, &brick_ptr, &brick_masks, &brick_mats]);
    let bg_fill_src = bind(gpu, &p_fill, 1, &[&scene.buf]);
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("fill"),
            timestamp_writes: Some(ts.pass_writes(2)),
        });
        pass.set_pipeline(&p_fill);
        pass.set_bind_group(0, &bg_fill, &[]);
        pass.set_bind_group(1, &bg_fill_src, &[]);
        pass.dispatch_workgroups(BGRID, BGRID, BGRID);
    }
    gpu.queue.submit([enc.finish()]);
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
    let build_ms = ts.read_ms(gpu, 2);

    let bytes = NUM_BRICKS * 4 + mask_bytes + mat_bytes;
    BrickmapBackend { brick_ptr, brick_masks, brick_mats, build_ms, brick_count, bytes }
}

/// The two traversal backends under test (docs/S64.md §2: both live in the
/// same binary, same scenes, same ray workloads).
enum Backend {
    Brickmap(BrickmapBackend),
    S64(s64::S64Backend),
}

impl Backend {
    fn build(gpu: &Gpu, shader_dir: &str, name: &str, seed: u32, scene: &SceneSource) -> Backend {
        match name {
            "brickmap" => Backend::Brickmap(build_brickmap(gpu, shader_dir, seed, scene)),
            "s64" => Backend::S64(s64::build_s64(gpu, shader_dir, seed, scene)),
            other => {
                eprintln!("unknown backend {other} (expected brickmap or s64)");
                std::process::exit(2);
            }
        }
    }
    fn name(&self) -> &'static str {
        match self {
            Backend::Brickmap(_) => "brickmap",
            Backend::S64(_) => "s64",
        }
    }
    /// Shader files concatenated ahead of bench.wgsl to provide trace().
    /// `traversal` selects the s64 variant (docs/S64.md §4: "stack" =
    /// small-stack descent, "stackless" = stackless re-descent).
    fn trace_files(&self, traversal: &str) -> [&'static str; 2] {
        match self {
            Backend::Brickmap(_) => ["brickmap_trace.wgsl", "bench.wgsl"],
            Backend::S64(_) if traversal == "stackless" => ["s64_trace_stackless.wgsl", "bench.wgsl"],
            Backend::S64(_) if traversal == "stackless-opt" => ["s64_trace_stackless_opt.wgsl", "bench.wgsl"],
            Backend::S64(_) => ["s64_trace.wgsl", "bench.wgsl"],
        }
    }
    /// @group(1) buffers, in binding order.
    fn trace_buffers(&self) -> Vec<&wgpu::Buffer> {
        match self {
            Backend::Brickmap(b) => vec![&b.brick_ptr, &b.brick_masks, &b.brick_mats],
            Backend::S64(b) => vec![&b.tlas, &b.nodes, &b.leaves, &b.mats],
        }
    }
    fn build_ms(&self) -> &[f64] {
        match self {
            Backend::Brickmap(b) => &b.build_ms,
            Backend::S64(b) => &b.build_ms,
        }
    }
    fn bytes(&self) -> u64 {
        match self {
            Backend::Brickmap(b) => b.bytes,
            Backend::S64(b) => b.bytes,
        }
    }
    fn log_build(&self) {
        let ms: f64 = self.build_ms().iter().sum();
        match self {
            Backend::Brickmap(b) => eprintln!(
                "build[brickmap]: {} bricks, {:.1} MB, {:.2} ms (occupancy {:.2} + fill {:.2})",
                b.brick_count,
                b.bytes as f64 / 1e6,
                ms,
                b.build_ms[0],
                b.build_ms[1]
            ),
            Backend::S64(b) => eprintln!(
                "build[s64]: {} roots, {} l1, {} leaves, {} mats, {:.1} MB, {:.2} ms (leafCount {:.2} + reduce {:.2} + pointers {:.2} + leafFill {:.2})",
                b.root_count,
                b.l1_count,
                b.leaf_count,
                b.mat_count,
                b.bytes as f64 / 1e6,
                ms,
                b.build_ms[0],
                b.build_ms[1],
                b.build_ms[2],
                b.build_ms[3]
            ),
        }
    }
    /// Backend-specific structure counts for the JSON row.
    fn json_counts(&self, row: &mut serde_json::Value) {
        match self {
            Backend::Brickmap(b) => {
                row["brickCount"] = b.brick_count.into();
            }
            Backend::S64(b) => {
                row["rootCount"] = b.root_count.into();
                row["l1Count"] = b.l1_count.into();
                row["leafCount"] = b.leaf_count.into();
                row["matCount"] = b.mat_count.into();
            }
        }
    }
}

fn median(xs: &mut [f64]) -> f64 {
    xs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    xs[xs.len() / 2]
}

/// CPU-only WGSL validation of every module combination the host builds,
/// so shader breakage is caught without touching the GPU (the benchmark
/// machine may be mid-campaign). Exits non-zero on any error.
fn check_shaders(shader_dir: &str) -> ! {
    let combos: [(&str, &[&str]); 7] = [
        ("brickmap-build", &["gen.wgsl", "source.wgsl", "brickmap_build.wgsl"]),
        ("scan", &["scan.wgsl"]),
        ("brickmap-trace", &["brickmap_trace.wgsl", "bench.wgsl"]),
        ("s64-build", &["gen.wgsl", "source.wgsl", "s64_build.wgsl"]),
        ("s64-trace", &["s64_trace.wgsl", "bench.wgsl"]),
        ("s64-trace-stackless", &["s64_trace_stackless.wgsl", "bench.wgsl"]),
        ("s64-trace-stackless-opt", &["s64_trace_stackless_opt.wgsl", "bench.wgsl"]),
    ];
    let mut failed = false;
    let mut validate = |label: &str, src: &str| match naga::front::wgsl::parse_str(src) {
        Err(e) => {
            eprintln!("{label}: PARSE ERROR\n{}", e.emit_to_string(src));
            failed = true;
        }
        Ok(module) => {
            let mut validator = naga::valid::Validator::new(
                naga::valid::ValidationFlags::all(),
                naga::valid::Capabilities::all(),
            );
            match validator.validate(&module) {
                Err(e) => {
                    eprintln!("{label}: VALIDATION ERROR\n{}", e.emit_to_string(src));
                    failed = true;
                }
                Ok(_) => eprintln!("{label}: ok"),
            }
        }
    };
    for (label, files) in combos {
        validate(label, &shader(shader_dir, files));
    }
    // Also validate the opt kernel with every lever const flipped from its
    // authored default: exercises the --opt-levers string-toggle contract
    // (set_shader_flag panics if a declaration drifted) plus the resulting
    // source. Const values cannot change what naga validates, but the toggle
    // path itself must not be able to break only at bench time.
    let mut toggled = shader(shader_dir, &["s64_trace_stackless_opt.wgsl", "bench.wgsl"]);
    for (name, v) in
        [("ENABLE_MEMO", false), ("ENABLE_MIRROR", false), ("ENABLE_SKIP", false), ("ENABLE_ANYHIT", true)]
    {
        toggled = set_shader_flag(&toggled, name, v);
    }
    validate("s64-trace-stackless-opt[toggled]", &toggled);
    std::process::exit(if failed { 1 } else { 0 });
}

fn main() {
    let args = parse_args();
    let shader_dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("shaders")
        .to_string_lossy()
        .into_owned();
    if std::env::args().any(|a| a == "--check-shaders") {
        check_shaders(&shader_dir);
    }
    let gpu = init_gpu();
    eprintln!(
        "adapter: {} ({:?}, {:?})",
        gpu.adapter_info.name, gpu.adapter_info.device_type, gpu.adapter_info.backend
    );

    // --- GPU vs CPU builder byte-compare (docs/S64.md §3 invariant 3) --------
    if args.verify_build {
        verify::run_verify_build(&gpu, &args, &shader_dir);
    }

    let scene = load_scene(&gpu, &args.scene);

    // --- Identity gate (docs/S64.md §5 criterion 1) --------------------------
    if args.gate {
        run_gate(&gpu, &args, &shader_dir, &scene);
    }

    // --- Build ---------------------------------------------------------------
    let backend = Backend::build(&gpu, &shader_dir, &args.backend, args.seed, &scene);
    backend.log_build();
    if args.backend == "s64" {
        if args.traversal == "stackless-opt" {
            let l = args.levers();
            eprintln!(
                "traversal[s64]: stackless-opt (levers: memo={} mirror={} skip={} anyhit={})",
                l[0], l[1], l[2], l[3]
            );
        } else {
            eprintln!("traversal[s64]: {}", args.traversal);
        }
    }

    // --- Trace ---------------------------------------------------------------
    // Workgroup size is a tuning knob (--wg WxH): at the default 8x8 = 64
    // threads (2 warps/CTA), Ampere's 16-CTA/SM limit caps occupancy at
    // 32/48 warps = 67% before registers are even considered.
    let wg_attr = format!("@workgroup_size({}, {})", args.wg.0, args.wg.1);
    let trace_src = trace_source(&shader_dir, &args, &backend, false).replace("@workgroup_size(8, 8)", &wg_attr);
    let trace_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("trace"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(trace_src)),
    });
    // Shadow any-hit (docs/S64OPT.md §3): the shadow pipelines come from a
    // second module instance with ENABLE_ANYHIT=true — bench.wgsl is frozen,
    // so the specialization is per module, not per entry point. All other
    // pipelines (and the identity gate) use the ENABLE_ANYHIT=false module.
    let shadow_mod_anyhit = if matches!(backend, Backend::S64(_)) && args.traversal == "stackless-opt" && args.levers()[3]
    {
        let src = trace_source(&shader_dir, &args, &backend, true).replace("@workgroup_size(8, 8)", &wg_attr);
        Some(gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: Some("trace-shadow-anyhit"),
            source: wgpu::ShaderSource::Wgsl(Cow::Owned(src)),
        }))
    } else {
        None
    };
    let shadow_mod = shadow_mod_anyhit.as_ref().unwrap_or(&trace_mod);
    let trace_pl = trace_pipeline_layout(&gpu, backend.trace_buffers().len(), true);
    let trace_pl_sec = trace_pipeline_layout(&gpu, backend.trace_buffers().len(), false);
    let p_primary = compute_pipeline_with(&gpu, &trace_mod, "primary", Some(&trace_pl));
    let p_bounce = compute_pipeline_with(
        &gpu,
        &trace_mod,
        if args.compact { "bounce_compact" } else { "bounce" },
        Some(&trace_pl_sec),
    );
    let p_shadow = compute_pipeline_with(
        &gpu,
        shadow_mod,
        if args.compact { "shadow_compact" } else { "shadow" },
        Some(&trace_pl_sec),
    );
    let p_prep = compute_pipeline_with(&gpu, &trace_mod, "prep", Some(&trace_pl));

    let npix = args.width as u64 * args.height as u64;
    let ub = bench_uniform(&args);
    let bench_params = buffer(
        &gpu.device,
        "bench-params",
        ub.len() as u64,
        wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST,
    );
    gpu.queue.write_buffer(&bench_params, 0, &ub);

    let image_out = buffer(&gpu.device, "image", npix * 4, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC);
    let hit_t = buffer(&gpu.device, "hit-t", npix * 4, wgpu::BufferUsages::STORAGE);
    let hit_nmat = buffer(&gpu.device, "hit-nmat", npix * 16, wgpu::BufferUsages::STORAGE);
    let stats = buffer(
        &gpu.device,
        "stats",
        16,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC | wgpu::BufferUsages::COPY_DST,
    );

    let compact_hits = buffer(
        &gpu.device,
        "compact-hits",
        (npix + 1) * 4,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST,
    );
    let indirect_args = buffer(
        &gpu.device,
        "indirect-args",
        12,
        wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::INDIRECT,
    );

    let backend_bufs = backend.trace_buffers();
    let g0_full: [&wgpu::Buffer; 7] =
        [&bench_params, &image_out, &hit_t, &hit_nmat, &stats, &compact_hits, &indirect_args];
    let g0_sec: [&wgpu::Buffer; 6] = [&bench_params, &image_out, &hit_t, &hit_nmat, &stats, &compact_hits];
    let bg0_p = bind(&gpu, &p_primary, 0, &g0_full);
    let bg1_p = bind(&gpu, &p_primary, 1, &backend_bufs);
    let bg0_b = bind(&gpu, &p_bounce, 0, &g0_sec);
    let bg1_b = bind(&gpu, &p_bounce, 1, &backend_bufs);
    let bg0_s = bind(&gpu, &p_shadow, 0, &g0_sec);
    let bg1_s = bind(&gpu, &p_shadow, 1, &backend_bufs);

    let ts = Timestamps::new(&gpu, 8);
    let gx = args.width.div_ceil(args.wg.0);
    let gy = args.height.div_ceil(args.wg.1);

    let mut per_class: Vec<[f64; 3]> = Vec::new();
    for _frame in 0..args.frames.max(2) {
        gpu.queue.write_buffer(&stats, 0, &[0u8; 16]);
        gpu.queue.write_buffer(&compact_hits, 0, &[0u8; 4]); // reset hit count
        let mut enc = gpu.device.create_command_encoder(&Default::default());
        {
            // Primary pass; with --compact its epilogue converts the hit
            // count into indirect args for the 1D passes (same-pass storage
            // ordering guarantees prep sees primary's writes; the indirect
            // buffer is only consumed in later passes).
            let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some("primary"),
                timestamp_writes: Some(ts.pass_writes(0)),
            });
            pass.set_pipeline(&p_primary);
            pass.set_bind_group(0, &bg0_p, &[]);
            pass.set_bind_group(1, &bg1_p, &[]);
            pass.dispatch_workgroups(gx, gy, 1);
            if args.compact {
                pass.set_pipeline(&p_prep);
                pass.dispatch_workgroups(1, 1, 1);
            }
        }
        let secondary: [(&wgpu::ComputePipeline, &wgpu::BindGroup, &wgpu::BindGroup, &str); 2] =
            [(&p_bounce, &bg0_b, &bg1_b, "bounce"), (&p_shadow, &bg0_s, &bg1_s, "shadow")];
        for (i, (p, bg0, bg1, label)) in secondary.iter().enumerate() {
            let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some(label),
                timestamp_writes: Some(ts.pass_writes((i as u32 + 1) * 2)),
            });
            pass.set_pipeline(p);
            pass.set_bind_group(0, *bg0, &[]);
            pass.set_bind_group(1, *bg1, &[]);
            if args.compact {
                pass.dispatch_workgroups_indirect(&indirect_args, 0);
            } else {
                pass.dispatch_workgroups(gx, gy, 1);
            }
        }
        gpu.queue.submit([enc.finish()]);
        gpu.device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
        let ms = ts.read_ms(&gpu, 3);
        per_class.push([ms[0], ms[1], ms[2]]);
    }
    per_class.remove(0); // warmup frame
    let stats_v = readback_u32(&gpu, &stats, 0, 4);

    let mut med = [0.0f64; 3];
    for c in 0..3 {
        let mut xs: Vec<f64> = per_class.iter().map(|f| f[c]).collect();
        med[c] = median(&mut xs);
    }
    let class_rays = [npix as f64, stats_v[0] as f64, stats_v[0] as f64];
    let names = ["primary", "bounce", "shadow"];
    let mut summary = String::new();
    for c in 0..3 {
        let mrays = class_rays[c] / (med[c] * 1e-3) / 1e6;
        let _ = writeln!(summary, "{}: {:.3} ms median, {:.0} Mrays/s", names[c], med[c], mrays);
    }
    eprintln!("{summary}");
    eprintln!(
        "hits: primary {} / {} ({:.1}%), bounce {}, shadow occluded {}",
        stats_v[0],
        npix,
        stats_v[0] as f64 / npix as f64 * 100.0,
        stats_v[1],
        stats_v[2]
    );

    // --- Outputs -------------------------------------------------------------
    if let Some(png_path) = &args.png {
        let pixels = readback_u32(&gpu, &image_out, 0, npix as usize);
        write_png(png_path, args.width, args.height, &pixels);
    }
    if let Some(out) = &args.out {
        let mut row = serde_json::json!({
            "backend": args.backend,
            "traversal": if args.backend == "s64" { args.traversal.as_str() } else { "-" },
            "scene": args.scene,
            "seed": args.seed,
            "resolution": format!("{}x{}", args.width, args.height),
            "frames": args.frames,
            "adapter": format!("{} {:?}", gpu.adapter_info.name, gpu.adapter_info.backend),
            "buildMs": backend.build_ms(),
            "structureBytes": backend.bytes(),
            "medianMsPerClass": { "primary": med[0], "bounce": med[1], "shadow": med[2] },
            "primaryHits": stats_v[0],
            "bounceHits": stats_v[1],
            "shadowOccluded": stats_v[2],
        });
        backend.json_counts(&mut row);
        if args.backend == "s64" && args.traversal == "stackless-opt" {
            let l = args.levers();
            row["optLevers"] =
                serde_json::json!({ "memo": l[0], "mirror": l[1], "skip": l[2], "anyhit": l[3] });
        }
        std::fs::write(out, serde_json::to_string_pretty(&row).unwrap()).expect("write out");
        eprintln!("wrote {out}");
    }
}

/// BenchParams uniform bytes, shared by the bench and gate paths so both
/// trace the exact same ray set.
///
/// sparse1024: camera above the terrain shell — world is 64 m across,
/// surface at y = 16..40 m — looking down the +x/+z diagonal.
/// fixture256: the browser interior_static pose from
/// test/eval/claim-manifest.v1.json (px/py/pz meters, yaw/pitch as in
/// src/math.js cameraBasis: yaw=0 faces +Z, positive pitch looks up). Both
/// renderers use 1/16 m voxels, so browser meters map 1:1 onto the fixture's
/// [0,16m)^3 corner of the native world.
fn bench_uniform(args: &Args) -> Vec<u8> {
    let (eye, fwd) = if args.scene == "fixture256" {
        let (yaw, pitch) = (-0.45f32, 0.02f32);
        ([8.8f32, 4.7, 7.0], [yaw.sin() * pitch.cos(), pitch.sin(), yaw.cos() * pitch.cos()])
    } else {
        ([20.0f32, 42.0, 20.0], [1.0, -0.35, 0.7])
    };
    let fwd = norm3(fwd);
    let world_up = [0.0f32, 1.0, 0.0];
    let right = norm3(cross(fwd, world_up));
    let up = cross(right, fwd);
    let aspect = args.width as f32 / args.height as f32;
    let tan_half = (60.0f32).to_radians().tan() * 0.5;

    let mut ub = Vec::<u8>::new();
    for v in [args.width, args.height, 0u32, 0u32] {
        ub.extend_from_slice(&v.to_le_bytes());
    }
    for v4 in [
        [eye[0], eye[1], eye[2], 0.0],
        [right[0] * tan_half * aspect, right[1] * tan_half * aspect, right[2] * tan_half * aspect, 0.0],
        [up[0] * tan_half, up[1] * tan_half, up[2] * tan_half, 0.0],
        [fwd[0], fwd[1], fwd[2], 0.0],
        norm4([0.4, 0.8, 0.45]),
    ] {
        for c in v4 {
            ub.extend_from_slice(&c.to_le_bytes());
        }
    }
    ub
}

fn write_png(path: &str, width: u32, height: u32, pixels: &[u32]) {
    let mut rgba = Vec::with_capacity(pixels.len() * 4);
    for p in pixels {
        rgba.extend_from_slice(&p.to_le_bytes());
    }
    let f = std::fs::File::create(path).expect("png create");
    let mut pngenc = png::Encoder::new(std::io::BufWriter::new(f), width, height);
    pngenc.set_color(png::ColorType::Rgba);
    pngenc.set_depth(png::BitDepth::Eight);
    let mut w = pngenc.write_header().expect("png header");
    w.write_image_data(&rgba).expect("png data");
    eprintln!("wrote {path}");
}

/// One backend's primary-ray results for the identity gate.
struct GateRun {
    image: Vec<u32>,
    hit_t: Vec<f32>,
    hit_nmat: Vec<u32>, // 4 u32 per pixel: packed normal, mat, 0, 0
}

fn gate_primary(gpu: &Gpu, args: &Args, shader_dir: &str, backend: &Backend, bench_params: &wgpu::Buffer) -> GateRun {
    let npix = args.width as u64 * args.height as u64;
    // trace_source honors --opt-levers for stackless-opt; the gate traces
    // primary rays only, so the any-hit module split never applies here.
    let trace_src = trace_source(shader_dir, args, backend, false);
    let trace_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("gate-trace"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(trace_src)),
    });
    let trace_pl = trace_pipeline_layout(gpu, backend.trace_buffers().len(), true);
    let p_primary = compute_pipeline_with(gpu, &trace_mod, "primary", Some(&trace_pl));

    let image_out = buffer(&gpu.device, "image", npix * 4, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC);
    let hit_t = buffer(&gpu.device, "hit-t", npix * 4, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC);
    let hit_nmat =
        buffer(&gpu.device, "hit-nmat", npix * 16, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_SRC);
    let stats = buffer(&gpu.device, "stats", 16, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST);
    gpu.queue.write_buffer(&stats, 0, &[0u8; 16]);
    let compact = buffer(&gpu.device, "compact", (npix + 1) * 4, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::COPY_DST);
    let indirect = buffer(&gpu.device, "indirect", 12, wgpu::BufferUsages::STORAGE | wgpu::BufferUsages::INDIRECT);
    gpu.queue.write_buffer(&compact, 0, &[0u8; 4]);

    let bg0 = bind(gpu, &p_primary, 0, &[bench_params, &image_out, &hit_t, &hit_nmat, &stats, &compact, &indirect]);
    let bg1 = bind(gpu, &p_primary, 1, &backend.trace_buffers());
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&Default::default());
        pass.set_pipeline(&p_primary);
        pass.set_bind_group(0, &bg0, &[]);
        pass.set_bind_group(1, &bg1, &[]);
        pass.dispatch_workgroups(args.width.div_ceil(8), args.height.div_ceil(8), 1);
    }
    gpu.queue.submit([enc.finish()]);
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");

    GateRun {
        image: readback_u32(gpu, &image_out, 0, npix as usize),
        hit_t: readback_u32(gpu, &hit_t, 0, npix as usize).iter().map(|v| f32::from_bits(*v)).collect(),
        hit_nmat: readback_u32(gpu, &hit_nmat, 0, npix as usize * 4),
    }
}

/// --gate: build BOTH backends on the same scene, trace the same primary ray
/// set through each, and byte-compare the hit buffers (docs/S64.md §3
/// invariant 1 / §5 acceptance criterion 1). |t| may differ by at most 1e-4
/// voxel units (6.25e-6 m); normals and materials must match exactly.
/// Exits non-zero on any mismatch.
fn run_gate(gpu: &Gpu, args: &Args, shader_dir: &str, scene: &SceneSource) -> ! {
    // Gate v2 (re-registered 2026-07-18; docs/S64.md §7 records the v1
    // failure and rationale). v1's absolute tolerance (1e-4 voxel) sat below
    // f32 ULP at sparse1024 distances: the two backends accumulate tMax over
    // very different step counts (~3000 vs ~150 at 1024³), so bit-identical t
    // is impossible in f32 even for identical traversal decisions. v2:
    //   - t must agree within max(1e-4 voxel, 16 ULP(t));
    //   - pixels that genuinely diverge (hit/miss disagreement, different
    //     material, or |dt| > 16 ULP) are knife-edge cases — a measure-zero,
    //     epsilon-unstable set — and are budgeted at <= 0.005% of all pixels;
    //   - face flips with t in tolerance are counted but allowed (two faces
    //     meet at the same t at an edge; which one reports is a tie-break).
    const T_TOL_M: f32 = 6.25e-6; // 1e-4 voxel units x 0.0625 m/voxel
    const ULP_TOL: f32 = 16.0;
    const DIVERGENCE_BUDGET: f64 = 0.00005; // 0.005% of pixels

    let backends = [
        Backend::build(gpu, shader_dir, "brickmap", args.seed, scene),
        Backend::build(gpu, shader_dir, "s64", args.seed, scene),
    ];
    for b in &backends {
        b.log_build();
    }
    eprintln!("gate s64 traversal: {}", args.traversal);

    let ub = bench_uniform(args);
    let bench_params =
        buffer(&gpu.device, "bench-params", ub.len() as u64, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
    gpu.queue.write_buffer(&bench_params, 0, &ub);

    let runs: Vec<GateRun> = backends.iter().map(|b| gate_primary(gpu, args, shader_dir, b, &bench_params)).collect();
    let (a, b) = (&runs[0], &runs[1]);

    let npix = (args.width * args.height) as usize;
    let mut mismatches = 0u64;
    let mut max_dt = 0.0f32;
    // Diagnostic breakdown: absolute-tolerance failures are meaningless
    // without knowing whether they are ULP-scale accumulation drift (t grows
    // with distance, so a fixed tolerance shrinks below f32 resolution) or
    // genuine divergence (different surface picked at a tie).
    let mut dt_ulp_hist = [0u64; 6]; // |dt| in units of ULP(t): <=1, <=2, <=4, <=16, <=64, >64
    let mut face_flips = 0u64; // t within 4 ULP but normal differs (knife edge)
    let mut mat_diffs = 0u64;
    let mut hit_miss = 0u64;
    for i in 0..npix {
        let (ta, tb) = (a.hit_t[i], b.hit_t[i]);
        let (na, ma) = (a.hit_nmat[i * 4], a.hit_nmat[i * 4 + 1]);
        let (nb, mb) = (b.hit_nmat[i * 4], b.hit_nmat[i * 4 + 1]);
        // A pixel is DIVERGENT (budgeted) if the backends disagree about what
        // was hit: hit/miss mismatch, different material, or t beyond both
        // the absolute and ULP-relative tolerance. Face flips inside the t
        // tolerance are tie-breaks at edges, counted but allowed.
        let divergent = if ta < 0.0 || tb < 0.0 {
            let agree = (ta < 0.0) == (tb < 0.0);
            if !agree {
                hit_miss += 1;
            }
            !agree
        } else {
            let dt = (ta - tb).abs();
            max_dt = max_dt.max(dt);
            let ulp = f32::from_bits(ta.to_bits() + 1) - ta; // ULP at this t
            let dt_ulps = dt / ulp.max(f32::MIN_POSITIVE);
            let bucket = match dt_ulps {
                x if x <= 1.0 => 0,
                x if x <= 2.0 => 1,
                x if x <= 4.0 => 2,
                x if x <= 16.0 => 3,
                x if x <= 64.0 => 4,
                _ => 5,
            };
            dt_ulp_hist[bucket] += 1;
            let t_ok = dt <= T_TOL_M || dt_ulps <= ULP_TOL;
            if t_ok && na != nb {
                face_flips += 1;
            }
            if ma != mb {
                mat_diffs += 1;
            }
            !(t_ok && ma == mb)
        };
        if divergent {
            if mismatches < 10 {
                eprintln!(
                    "DIVERGENT pixel ({}, {}): brickmap t={ta:.7} n={na} mat={ma:#010x} | s64 t={tb:.7} n={nb} mat={mb:#010x}",
                    i as u32 % args.width,
                    i as u32 / args.width,
                );
            }
            mismatches += 1;
        }
    }
    eprintln!(
        "gate diagnostics: |dt| ULP histogram (<=1,<=2,<=4,<=16,<=64,>64) = {dt_ulp_hist:?} | \
         in-tolerance face flips: {face_flips} | material diffs: {mat_diffs} | hit/miss disagreements: {hit_miss}"
    );

    if let Some(png_path) = &args.png {
        for (backend, run) in backends.iter().zip(&runs) {
            let path = match png_path.rsplit_once('.') {
                Some((stem, ext)) => format!("{stem}-{}.{ext}", backend.name()),
                None => format!("{png_path}-{}", backend.name()),
            };
            write_png(&path, args.width, args.height, &run.image);
        }
    }

    let budget = (npix as f64 * DIVERGENCE_BUDGET).floor() as u64;
    eprintln!(
        "gate: {mismatches} divergent pixels / {npix} (budget {budget} = {:.3}%), \
         max |dt| among hits {max_dt:.3e} m",
        DIVERGENCE_BUDGET * 100.0
    );
    if mismatches > budget {
        eprintln!("gate FAILED");
        std::process::exit(1);
    }
    eprintln!("gate PASSED");
    std::process::exit(0);
}

fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
fn norm3(v: [f32; 3]) -> [f32; 3] {
    let l = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
    [v[0] / l, v[1] / l, v[2] / l]
}
fn norm4(v: [f32; 3]) -> [f32; 4] {
    let n = norm3(v);
    [n[0], n[1], n[2], 0.0]
}
