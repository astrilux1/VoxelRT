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

const GRID: u32 = 1024;
const BRICK: u32 = 8;
const BGRID: u32 = GRID / BRICK; // 128
const NUM_BRICKS: u64 = (BGRID as u64) * (BGRID as u64) * (BGRID as u64);

struct Args {
    backend: String,
    scene: String,
    width: u32,
    height: u32,
    frames: u32,
    seed: u32,
    out: Option<String>,
    png: Option<String>,
}

fn parse_args() -> Args {
    let mut a = Args {
        backend: "brickmap".into(),
        scene: "sparse1024".into(),
        width: 1920,
        height: 1080,
        frames: 5,
        seed: 1,
        out: None,
        png: Some("native-bench.png".into()),
    };
    let argv: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < argv.len() {
        let k = argv[i].clone();
        if k == "--check-shaders" {
            i += 1; // value-less flag, handled in main
            continue;
        }
        let v = argv.get(i + 1).cloned().unwrap_or_else(|| {
            eprintln!("missing value for {k}");
            std::process::exit(2);
        });
        match k.as_str() {
            "--backend" => a.backend = v,
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
            _ => {
                eprintln!("unknown arg {k}");
                std::process::exit(2);
            }
        }
        i += 2;
    }
    a
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
    gpu.device.create_compute_pipeline(&wgpu::ComputePipelineDescriptor {
        label: Some(entry),
        layout: None,
        module,
        entry_point: Some(entry),
        compilation_options: Default::default(),
        cache: None,
    })
}

fn bind(gpu: &Gpu, pipeline: &wgpu::ComputePipeline, group: u32, buffers: &[&wgpu::Buffer]) -> wgpu::BindGroup {
    let layout = pipeline.get_bind_group_layout(group);
    let entries: Vec<wgpu::BindGroupEntry> = buffers
        .iter()
        .enumerate()
        .map(|(i, b)| wgpu::BindGroupEntry { binding: i as u32, resource: b.as_entire_binding() })
        .collect();
    gpu.device.create_bind_group(&wgpu::BindGroupDescriptor { label: None, layout: &layout, entries: &entries })
}

/// GPU exclusive scan over `buf` (n u32s), in place. Returns the total sum.
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
    let p_blocks = compute_pipeline(gpu, scan_mod, "scan_blocks");
    let p_sums = compute_pipeline(gpu, scan_mod, "scan_sums");
    let p_add = compute_pipeline(gpu, scan_mod, "scan_add");
    // Read the last element before scanning so the total can be recovered
    // (total = last exclusive prefix + last input).
    let last_in = readback_u32(gpu, buf, (n as u64 - 1) * 4, 1)[0];

    let bg_blocks = bind(gpu, &p_blocks, 0, &[&params, buf, &block_sums]);
    let bg_sums = bind(gpu, &p_sums, 0, &[&params, buf, &block_sums]);
    let bg_add = bind(gpu, &p_add, 0, &[&params, buf, &block_sums]);
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&Default::default());
        pass.set_pipeline(&p_blocks);
        pass.set_bind_group(0, &bg_blocks, &[]);
        pass.dispatch_workgroups(blocks, 1, 1);
        pass.set_pipeline(&p_sums);
        pass.set_bind_group(0, &bg_sums, &[]);
        pass.dispatch_workgroups(1, 1, 1);
        pass.set_pipeline(&p_add);
        pass.set_bind_group(0, &bg_add, &[]);
        pass.dispatch_workgroups(blocks, 1, 1);
    }
    gpu.queue.submit([enc.finish()]);
    let last_out = readback_u32(gpu, buf, (n as u64 - 1) * 4, 1)[0];
    last_out + last_in
}

struct BrickmapBackend {
    brick_ptr: wgpu::Buffer,
    brick_masks: wgpu::Buffer,
    brick_mats: wgpu::Buffer,
    build_ms: Vec<f64>,
    brick_count: u32,
    bytes: u64,
}

fn build_brickmap(gpu: &Gpu, shader_dir: &str, seed: u32) -> BrickmapBackend {
    let gen_src = shader(shader_dir, &["gen.wgsl", "brickmap_build.wgsl"]);
    let gen_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("brickmap-build"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(gen_src)),
    });
    let scan_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("scan"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(shader(shader_dir, &["scan.wgsl"]))),
    });

    let params = buffer(&gpu.device, "gen-params", 16, wgpu::BufferUsages::UNIFORM | wgpu::BufferUsages::COPY_DST);
    gpu.queue.write_buffer(&params, 0, bytemuck::cast_slice(&[seed, GRID.trailing_zeros(), 0u32, 0u32]));

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
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("occupancy"),
            timestamp_writes: Some(ts.pass_writes(0)),
        });
        pass.set_pipeline(&p_occ);
        pass.set_bind_group(0, &bg_occ, &[]);
        pass.dispatch_workgroups(BGRID, BGRID, BGRID);
    }
    enc.copy_buffer_to_buffer(&occupied, 0, &slots, 0, NUM_BRICKS * 4);
    gpu.queue.submit([enc.finish()]);

    // Scan slots in place -> allocation indices; total = allocated brick count.
    let brick_count = gpu_scan(gpu, &scan_mod, &slots, NUM_BRICKS as u32);
    assert!(brick_count > 0, "scene generated zero bricks");
    let mask_bytes = brick_count as u64 * 16 * 4;
    let mat_bytes = brick_count as u64 * 512 * 4;

    let brick_ptr = buffer(&gpu.device, "brick-ptr", NUM_BRICKS * 4, wgpu::BufferUsages::STORAGE);
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
    let mut enc = gpu.device.create_command_encoder(&Default::default());
    {
        let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
            label: Some("fill"),
            timestamp_writes: Some(ts.pass_writes(2)),
        });
        pass.set_pipeline(&p_fill);
        pass.set_bind_group(0, &bg_fill, &[]);
        pass.dispatch_workgroups(BGRID, BGRID, BGRID);
    }
    gpu.queue.submit([enc.finish()]);
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).expect("poll");
    let build_ms = ts.read_ms(gpu, 2);

    let bytes = NUM_BRICKS * 4 + mask_bytes + mat_bytes;
    BrickmapBackend { brick_ptr, brick_masks, brick_mats, build_ms, brick_count, bytes }
}

fn median(xs: &mut [f64]) -> f64 {
    xs.sort_by(|a, b| a.partial_cmp(b).unwrap());
    xs[xs.len() / 2]
}

/// CPU-only WGSL validation of every module combination the host builds,
/// so shader breakage is caught without touching the GPU (the benchmark
/// machine may be mid-campaign). Exits non-zero on any error.
fn check_shaders(shader_dir: &str) -> ! {
    let combos: [(&str, &[&str]); 3] = [
        ("brickmap-build", &["gen.wgsl", "brickmap_build.wgsl"]),
        ("scan", &["scan.wgsl"]),
        ("brickmap-trace", &["brickmap_trace.wgsl", "bench.wgsl"]),
    ];
    let mut failed = false;
    for (label, files) in combos {
        let src = shader(shader_dir, files);
        match naga::front::wgsl::parse_str(&src) {
            Err(e) => {
                eprintln!("{label}: PARSE ERROR\n{}", e.emit_to_string(&src));
                failed = true;
            }
            Ok(module) => {
                let mut validator = naga::valid::Validator::new(
                    naga::valid::ValidationFlags::all(),
                    naga::valid::Capabilities::all(),
                );
                match validator.validate(&module) {
                    Err(e) => {
                        eprintln!("{label}: VALIDATION ERROR\n{}", e.emit_to_string(&src));
                        failed = true;
                    }
                    Ok(_) => eprintln!("{label}: ok"),
                }
            }
        }
    }
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

    if args.scene != "sparse1024" {
        eprintln!("scene {} not implemented yet (fixture256 lands with the identity gate)", args.scene);
        std::process::exit(2);
    }

    // --- Build ---------------------------------------------------------------
    let backend = match args.backend.as_str() {
        "brickmap" => build_brickmap(&gpu, &shader_dir, args.seed),
        other => {
            eprintln!("backend {other} not implemented yet");
            std::process::exit(2);
        }
    };
    eprintln!(
        "build: {} bricks, {:.1} MB, occupancy {:.2} ms + fill {:.2} ms",
        backend.brick_count,
        backend.bytes as f64 / 1e6,
        backend.build_ms[0],
        backend.build_ms[1]
    );

    // --- Trace ---------------------------------------------------------------
    let trace_src = shader(&shader_dir, &["brickmap_trace.wgsl", "bench.wgsl"]);
    let trace_mod = gpu.device.create_shader_module(wgpu::ShaderModuleDescriptor {
        label: Some("trace"),
        source: wgpu::ShaderSource::Wgsl(Cow::Owned(trace_src)),
    });
    let p_primary = compute_pipeline(&gpu, &trace_mod, "primary");
    let p_bounce = compute_pipeline(&gpu, &trace_mod, "bounce");
    let p_shadow = compute_pipeline(&gpu, &trace_mod, "shadow");

    let npix = args.width as u64 * args.height as u64;
    // Camera: above the terrain shell (world is 64 m across, surface at
    // y = 16..40 m), looking down the +x/+z diagonal.
    let eye = [20.0f32, 42.0, 20.0];
    let fwd = norm3([1.0, -0.35, 0.7]);
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

    let backend_bufs = [&backend.brick_ptr, &backend.brick_masks, &backend.brick_mats];
    let bg0_p = bind(&gpu, &p_primary, 0, &[&bench_params, &image_out, &hit_t, &hit_nmat, &stats]);
    let bg1_p = bind(&gpu, &p_primary, 1, &backend_bufs);
    let bg0_b = bind(&gpu, &p_bounce, 0, &[&bench_params, &image_out, &hit_t, &hit_nmat, &stats]);
    let bg1_b = bind(&gpu, &p_bounce, 1, &backend_bufs);
    let bg0_s = bind(&gpu, &p_shadow, 0, &[&bench_params, &image_out, &hit_t, &hit_nmat, &stats]);
    let bg1_s = bind(&gpu, &p_shadow, 1, &backend_bufs);

    let ts = Timestamps::new(&gpu, 8);
    let gx = args.width.div_ceil(8);
    let gy = args.height.div_ceil(8);

    let mut per_class: Vec<[f64; 3]> = Vec::new();
    for _frame in 0..args.frames.max(2) {
        gpu.queue.write_buffer(&stats, 0, &[0u8; 16]);
        let mut enc = gpu.device.create_command_encoder(&Default::default());
        let passes: [(&wgpu::ComputePipeline, &wgpu::BindGroup, &wgpu::BindGroup, &str); 3] = [
            (&p_primary, &bg0_p, &bg1_p, "primary"),
            (&p_bounce, &bg0_b, &bg1_b, "bounce"),
            (&p_shadow, &bg0_s, &bg1_s, "shadow"),
        ];
        for (i, (p, bg0, bg1, label)) in passes.iter().enumerate() {
            let mut pass = enc.begin_compute_pass(&wgpu::ComputePassDescriptor {
                label: Some(label),
                timestamp_writes: Some(ts.pass_writes(i as u32 * 2)),
            });
            pass.set_pipeline(p);
            pass.set_bind_group(0, *bg0, &[]);
            pass.set_bind_group(1, *bg1, &[]);
            pass.dispatch_workgroups(gx, gy, 1);
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
        let mut rgba = Vec::with_capacity(pixels.len() * 4);
        for p in &pixels {
            rgba.extend_from_slice(&p.to_le_bytes());
        }
        let f = std::fs::File::create(png_path).expect("png create");
        let mut pngenc = png::Encoder::new(std::io::BufWriter::new(f), args.width, args.height);
        pngenc.set_color(png::ColorType::Rgba);
        pngenc.set_depth(png::BitDepth::Eight);
        let mut w = pngenc.write_header().expect("png header");
        w.write_image_data(&rgba).expect("png data");
        eprintln!("wrote {png_path}");
    }
    if let Some(out) = &args.out {
        let row = serde_json::json!({
            "backend": args.backend,
            "scene": args.scene,
            "seed": args.seed,
            "resolution": format!("{}x{}", args.width, args.height),
            "frames": args.frames,
            "adapter": format!("{} {:?}", gpu.adapter_info.name, gpu.adapter_info.backend),
            "buildMs": backend.build_ms,
            "brickCount": backend.brick_count,
            "structureBytes": backend.bytes,
            "medianMsPerClass": { "primary": med[0], "bounce": med[1], "shadow": med[2] },
            "primaryHits": stats_v[0],
            "bounceHits": stats_v[1],
            "shadowOccluded": stats_v[2],
        });
        std::fs::write(out, serde_json::to_string_pretty(&row).unwrap()).expect("write out");
        eprintln!("wrote {out}");
    }
    let _ = BRICK; // layout constant referenced by shaders; kept for clarity
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
