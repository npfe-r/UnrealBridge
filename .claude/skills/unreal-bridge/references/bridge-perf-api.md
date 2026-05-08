# bridge-perf-api

`unreal.UnrealBridgePerfLibrary` — structured performance snapshots. Replaces parsing `stat unit` text output with typed UPROPERTY structs.

Values come from engine globals on the GameThread:
- `FStatUnitData` from the active level viewport (smoothed running averages) → `GetFrameTiming`
- `GNumDrawCallsRHI` / `GNumPrimitivesDrawnRHI` (RHI, summed across `MAX_NUM_GPUS`) → `GetRenderCounters`
- `FPlatformMemory::GetStats()` + `FPlatformMemory::GetConstants()` → `GetMemoryStats`
- `TObjectIterator<UObject>` aggregated by `UClass` → `GetUObjectStats`

All functions are cheap except `GetUObjectStats` — see the cost note per function.

---

## get_frame_timing() -> FBridgeFrameTiming

Frame-time breakdown (FPS, game/render/GPU ms). Always pulls from the raw `GGameThreadTime` / `GRenderThreadTime` / `RHIGetGPUFrameCycles` globals (updated every frame by `FViewport::Draw`, independent of any stat display). When the active level viewport has `stat unit` enabled — detected by checking `FStatUnitData::FrameTime > 0` — the smoothed running averages override the raw values for stability.

| Field | Type | Notes |
|---|---|---|
| `fps` | float | `GAverageFPS`. 0 until the first full frame. |
| `frame_ms` | float | `FStatUnitData::FrameTime` when smoothed; else `1000/fps` (or `GAverageMS` if FPS is 0). |
| `game_thread_ms` | float | `GGameThreadTime` in ms; overridden by `FStatUnitData::GameThreadTime` when smoothed. |
| `render_thread_ms` | float | `GRenderThreadTime` in ms; overridden by `FStatUnitData::RenderThreadTime` when smoothed. |
| `gpu_ms` | float | `RHIGetGPUFrameCycles` summed across `GNumExplicitGPUsForRendering`; overridden by `FStatUnitData::GPUFrameTime` (summed) when smoothed. |
| `rhi_ms` | float | RHI translation time. 0 on the raw path — only populated when smoothed=true. |
| `delta_seconds` | float | `FApp::GetDeltaTime()` for the most recent frame. |
| `frame_number` | int64 | `GFrameCounter`. |
| `smoothed` | bool | True when `stat unit` is active on a viewport and values came from `FStatUnitData`. False = raw per-frame globals (no running average). |

**Cost** — O(1). Safe to poll per frame.

**Example**
```python
import unreal
t = unreal.UnrealBridgePerfLibrary.get_frame_timing()
print(f"FPS={t.fps:.1f} GT={t.game_thread_ms:.2f}ms RT={t.render_thread_ms:.2f}ms GPU={t.gpu_ms:.2f}ms smoothed={t.smoothed}")
```

**Pitfalls**
- When the editor is unfocused / minimised, UE throttles the tick to ~3 FPS by default (`Editor Preferences → Use Less CPU when in Background`). Timings aren't wrong — the editor really is running that slow. Focus the window or disable the throttle to measure real performance.
- `render_thread_ms` can be 0 on the raw path when the RT spent all its time idle. Not a bug.
- `rhi_ms` on the raw path is always 0. To read actual RHI time, turn on `stat unit` in the active viewport — then `smoothed=true` and `rhi_ms` is populated.
- PIE drives its own rendering — timings inside PIE reflect the PIE viewport, not the editor viewport.

---

## get_render_counters() -> FBridgeRenderCounters

Draw call and primitive counts for the most recently rendered frame.

| Field | Type | Notes |
|---|---|---|
| `draw_calls` | int32 | `GNumDrawCallsRHI` summed across `MAX_NUM_GPUS`. |
| `primitives_drawn` | int32 | `GNumPrimitivesDrawnRHI` summed across `MAX_NUM_GPUS`. |
| `num_gpus` | int32 | `GNumExplicitGPUsForRendering`. 1 on most desktop builds. |

**Cost** — O(1). Safe to poll per frame.

**Example**
```python
c = unreal.UnrealBridgePerfLibrary.get_render_counters()
print(f"draws={c.draw_calls} prims={c.primitives_drawn}")
```

**Pitfalls**
- These are counters for the most recently *submitted* frame. If the editor hasn't rendered (minimised), values are stale.
- Capped at `MAX_int32` in the unlikely event of overflow — use the aggregate snapshot's `frame_number` to detect counter saturation across a long run.

---

## get_memory_stats() -> FBridgeMemoryStats

Process + system memory in mebibytes (MiB = 1024 × 1024 bytes).

| Field | Type | Notes |
|---|---|---|
| `used_physical_mb` | int64 | Process working set. |
| `used_virtual_mb` | int64 | Process virtual commit. |
| `peak_used_physical_mb` | int64 | Peak working set observed this session. |
| `peak_used_virtual_mb` | int64 | Peak virtual commit observed this session. |
| `available_physical_mb` | int64 | System-wide available physical RAM. |
| `available_virtual_mb` | int64 | System-wide available virtual. |
| `total_physical_mb` | int64 | Total physical RAM on the machine. |

**Cost** — O(1). Safe to poll per second.

**Example**
```python
m = unreal.UnrealBridgePerfLibrary.get_memory_stats()
print(f"editor using {m.used_physical_mb} MiB (peak {m.peak_used_physical_mb}) / {m.total_physical_mb} total")
```

---

## get_u_object_stats(top_n=20) -> FBridgeUObjectStats

Top-N `UClass` types ranked by live UObject count, plus totals.

| Field | Type | Notes |
|---|---|---|
| `total_objects` | int32 | Every live UObject walked. |
| `unique_classes` | int32 | Distinct `UClass` types seen. |
| `top_classes` | array of `FBridgeUObjectClassCount` | `{class_name, count}`, descending by `count`. |

**Cost** — O(N) in live UObjects. A typical mid-sized editor session has 300 k – 2 M UObjects, so this call costs **10 – 200 ms** on the GameThread. **Don't poll per frame.** Use for "is memory climbing?" diagnostics, memory leak hunting, or baseline snapshots between operations.

**Parameters**
- `top_n` (int32): number of classes to return. Clamped to `[1, 200]`. Default 20.

**Example**
```python
u = unreal.UnrealBridgePerfLibrary.get_u_object_stats(10)
print(f"{u.total_objects} UObjects across {u.unique_classes} classes")
for row in u.top_classes:
    print(f"  {row.count:>8}  {row.class_name}")
```

**Pitfalls**
- The walk is not atomic — during a GC pass UObject count can drop mid-iteration. For stable baselines, call before any PIE and avoid right after `CollectGarbage`.
- Reported classes are the *leaf* class of each object, not the nearest `UClass` in a hierarchy you care about. To bucket "all BP-derived Actors" you need to post-process `top_classes` yourself.

---

## get_perf_snapshot(include_uobject_stats=False, uobject_top_n=20) -> FBridgePerfSnapshot

One-call aggregate for regression tests / CI-style sampling.

| Field | Type | Notes |
|---|---|---|
| `timing` | `FBridgeFrameTiming` | See `get_frame_timing`. |
| `render` | `FBridgeRenderCounters` | See `get_render_counters`. |
| `memory` | `FBridgeMemoryStats` | See `get_memory_stats`. |
| `u_objects` | `FBridgeUObjectStats` | Zero-filled when `include_uobject_stats=False` (default). |
| `capture_time_utc` | str | ISO-8601 UTC timestamp — handy for delta-comparison across snapshots. |
| `engine_version` | str | e.g. `"5.7.0-0+UE5"`. |
| `was_in_pie` | bool | True when `GEditor->PlayWorld` was non-null at capture time. |

**Parameters**
- `include_uobject_stats` (bool): when true, also runs `get_u_object_stats(uobject_top_n)` (slow — see above). Default false.
- `uobject_top_n` (int32): forwarded to `get_u_object_stats` when enabled.

**Cost**
- With `include_uobject_stats=False`: O(1), microseconds. OK to poll per second.
- With `include_uobject_stats=True`: 10-200 ms. Use for keyframed baselines, not hot-loop polling.

**Example — cheap snapshot for regression comparison**
```python
import unreal, json
s = unreal.UnrealBridgePerfLibrary.get_perf_snapshot(False)
print(f"[{s.capture_time_utc}] FPS={s.timing.fps:.1f} draws={s.render.draw_calls} mem={s.memory.used_physical_mb}MiB pie={s.was_in_pie}")
```

**Example — full snapshot with UObject histogram**
```python
s = unreal.UnrealBridgePerfLibrary.get_perf_snapshot(True, 15)
print(f"[{s.capture_time_utc}] UObjects={s.u_objects.total_objects} across {s.u_objects.unique_classes} classes")
for row in s.u_objects.top_classes[:5]:
    print(f"  {row.count:>8}  {row.class_name}")
```

---

## parse_cook_trace_to_summary(utrace_path, top_n=50) -> FBridgePerfCookSummary

**(M6-3)** Parse a cook trace's `ICookProfilerProvider` data. Trace must contain the `cook` channel — typically captured during a cook commandlet:

```
UnrealEditor-Cmd.exe MyGame.uproject -run=Cook -targetplatform=Windows -trace=cook,cpu,frame -tracefile=MyCook.utrace
```

| Field | Type | Notes |
|---|---|---|
| `trace_path` / `file_size_bytes` | str / int64 | Echoed back. |
| `has_events` | bool | False when the trace lacks `cook` channel. |
| `package_count` | int32 | Number of packages observed. |
| `total_cook_time_seconds` | double | Sum of every package's `total_cook_time_ms`. |
| `packages` | array of `FBridgePerfCookRow` | Top-N by `total_cook_time_ms` desc. |
| `success` / `error` | bool / str | |

### `FBridgePerfCookRow`

| Field | Type | Notes |
|---|---|---|
| `package_name` | str | Package path (e.g. `"/Game/Maps/Forest"`). |
| `asset_class` | str | Top-level asset class (`"Texture2D"`, `"Material"`, …). |
| `total_cook_time_ms` | double | Sum of the four phases below. |
| `load_time_ms` | double | Inclusive `LoadPackage` time. |
| `save_time_ms` | double | Inclusive `SavePackage` time. |
| `begin_cache_cooked_platform_data_ms` | double | Inclusive `BeginCacheForCookedPlatformData` time — typically dominated by shader / DDC compile for Material packages. |
| `is_cached_cooked_platform_data_loaded_ms` | double | Inclusive `IsCachedCookedPlatformDataLoaded` poll time. |

**Use case**: 4-hour cook attribution. Typically the worst offenders are Material-class packages where `begin_cache_cooked_platform_data_ms` dominates (shader compile).

---

## parse_net_trace_to_summary(utrace_path) -> FBridgePerfNetSummary

**(M6-2)** Parse a `.utrace` file's network profiler data. Trace must contain the `net` channel; without it `net_trace_version` is 0 and `has_events` is False.

| Field | Type | Notes |
|---|---|---|
| `trace_path` / `file_size_bytes` | str / int64 | Echoed back. |
| `net_trace_version` | int32 | NetTrace stream version. 0 = no net data in this trace. |
| `has_events` | bool | True when version > 0 + ≥ 1 game instance. |
| `game_instances` | array of `FBridgePerfNetGameInstance` | One row per game instance (server / client / dedicated). |
| `success` / `error` | bool / str | |

### `FBridgePerfNetGameInstance`

| Field | Type | Notes |
|---|---|---|
| `instance_name` | str | Engine-supplied instance label. |
| `is_server` | bool | True for the listen/dedicated server side. |
| `is_using_iris_replication` | bool | Set when the instance routes through the Iris replication system. |
| `object_count` | int32 | Number of replicated objects observed. |
| `connections` | array of `FBridgePerfNetConnection` | One row per connection on the instance. |

### `FBridgePerfNetConnection`

| Field | Type | Notes |
|---|---|---|
| `name` | str | Connection name. |
| `address_string` | str | Address (IP:port for live connections). |
| `connection_id` | int32 | Engine-internal id. |
| `incoming_packet_count` / `outgoing_packet_count` | int64 | Packet counts per direction. |
| `incoming_bytes` / `outgoing_bytes` | int64 | Sum of `TotalPacketSizeInBytes` per direction. |

**MVP scope**: per-connection traffic totals. Per-actor replication breakdown + most-expensive-RPC ranking is deferred — would require walking every packet content event and resolving `ObjectInstanceIndex` against the FNetProfilerObjectInstance table.

---

## parse_alloc_trace_to_summary(utrace_path) -> FBridgePerfAllocSummary

**(M6-1)** Parse a `.utrace` file's allocation provider into a structured summary. Trace **must** contain the `memalloc` channel **AND** be captured from engine startup (`-trace=memalloc,frame,cpu` on the editor command line) — `Trace.Start memalloc=on` at runtime cannot retroactively install the malloc hooks needed to record events.

| Field | Type | Notes |
|---|---|---|
| `trace_path` / `file_size_bytes` | str / int64 | Echoed back. |
| `has_events` | bool | False when the alloc provider is empty (channel missing or runtime-only enable). Other fields will be 0. |
| `peak_total_allocated_bytes` | int64 | Peak commit across the trace (`MaxTotalAllocatedMemory` timeline). |
| `peak_live_allocations` | int64 | Peak number of simultaneously-live allocations. |
| `total_alloc_events` / `total_free_events` | int64 | Cumulative event counts. |
| `alloc_free_delta` | int64 | `alloc - free`. Editor traces always end > 0 (editor doesn't free everything). Compare across two captures of the same workload to detect leaks. |
| `tags` | array of `FBridgePerfAllocTag` | Full LLM tag inventory (`{id, parent_id, name, full_path}`; root tags have `parent_id=-1`). |
| `success` | bool | True on success; on failure populates `error`. |

**Phase 2 deferred**: per-allocation top-N "unfreed by size + callstack" requires the alloc provider's async `StartQuery` / `PollQuery` / `NextResult` machinery. Not in this MVP — the aggregate data above is enough to detect "did the run leak", peak commit, and which subsystems are tagged.

**Pitfalls**
- Mid-run `Trace.Start memalloc=on` produces empty alloc data (no startup hooks).
- Alloc traces are large — 1 GB of memory activity ~= multi-GB trace. Cap trace duration.

---

## analyze_all_materials(top_n=30) -> FBridgeAllMaterialsAnalysis

**(M7-4)** Walks every UMaterial master asset via AssetRegistry, loads each, counts expressions by class (TextureSample / Custom / StaticSwitch / parameter types) and surfaces the heaviest ones.

| Top-level field | Type | Notes |
|---|---|---|
| `total_materials` | int32 | Master materials walked. |
| `total_material_instances` | int32 | UMaterialInstance assets discovered (not analysed individually). |
| `rows` | array of `FBridgeMaterialPerfRow` | Top-N by `complexity_score` desc. |

### `FBridgeMaterialPerfRow`

| Field | Type | Notes |
|---|---|---|
| `material_path` | str | Asset path. |
| `blend_mode` / `shading_model` / `material_domain` | str | Pretty enum names (e.g. `"BLEND_Masked"`, `"MSM_DefaultLit"`). |
| `two_sided` / `used_with_skeletal_mesh` / `used_with_static_lighting` | bool | Surface flags. |
| `expression_count` | int32 | Total expression nodes in the material graph. |
| `texture_sample_count` | int32 | Includes parameter variants (TextureSampleParameter2D etc). |
| `custom_expression_count` | int32 | UMaterialExpressionCustom (HLSL inline). |
| `static_switch_count` | int32 | UMaterialExpressionStaticSwitch* (compile-time branches). |
| `scalar_parameter_count` / `vector_parameter_count` / `texture_parameter_count` | int32 | Parameter inventories. |
| `complexity_score` | int32 | `expression_count + 4×texture_sample_count + 8×custom_expression_count`. Heuristic only — not a real GPU instruction count. |

**Cost** — O(N) load + O(expressions) per material; loads materials that aren't already in memory. Can be slow on large projects (tens of seconds for thousands of materials).

**Scope limitation** — does NOT report real GPU instruction counts. That requires `FMaterialStatsUtils::ExtractMatertialStatsInfo` which is internal to the Material Editor module's compile pipeline. The complexity_score is a structural heuristic that ranks the worst graphs first; for actual instruction counts open the asset in Material Editor and read the stats panel.

---

## get_per_pass_gpu_timings() -> FBridgeGpuPassTimings

**(M7-3)** Per-pass GPU timings via `FRealtimeGPUProfiler::FetchPerfByDescription`. One row per registered GPU stat scope (BasePass / ShadowDepths / Lumen / Translucency / PostProcess / etc.) with average / min / max ms over the profiler's rolling 64-frame history.

| Top-level field | Type | Notes |
|---|---|---|
| `available` | bool | False on the new RHI GPU profiler path (`RHI_NEW_GPU_PROFILER=1`) — fall back to Insights with `gpu` + `rdg` channels. |
| `sum_average_ms` | double | Sum of average ms across all rows. Approximates last-frame "scene rendering" GPU time; not a literal frame total because passes can overlap on multi-queue GPUs. |
| `pass_count` | int32 | Number of pass rows. |
| `passes` | array of `FBridgeGpuPassTiming` | Sorted by `average_ms` desc. |
| `diagnostic` | str | Why `available=false` when applicable. |

### `FBridgeGpuPassTiming`

| Field | Type | Notes |
|---|---|---|
| `pass_name` | str | Pass description as registered. |
| `gpu_index` | int32 | GPU index (0 in single-GPU). |
| `average_ms` / `min_ms` / `max_ms` | double | Profiler's running stats (microseconds → ms). |

**UE 5.7 reality check** — UE 5.7 ships with `RHI_NEW_GPU_PROFILER=1` enabled by default and the legacy `FRealtimeGPUProfiler` table is empty. The new profiler stores per-pass data via `UE::RHI::GPUProfiler::FGPUStat::FStatInstance` (keyed by queue × Busy/Wait/Idle), but exposing it requires hooking the standard STATS system or the `gpu` trace channel — out of scope for a single-call live query.

**Workaround for live per-pass GPU on 5.7**: capture a trace with `cpu`, `gpu`, `frame` channels, then call `parse_trace_to_summary` — its `gpu_hot_scopes` field carries the same data resolved offline. This op is best when 5.8+ stabilises the new-profiler query API.

---

## get_render_target_memory(top_n=30) -> FBridgeRenderTargetMemory

**(M7-2)** Aggregate memory of `UTextureRenderTarget*` objects in memory. Walks `TObjectIterator<UTexture>` filtering by `IsA<UTextureRenderTarget>` (the abstract base doesn't iterate directly).

| Top-level field | Type | Notes |
|---|---|---|
| `total_bytes` | int64 | Sum across all RTs walked. |
| `render_target_count` | int32 | Number of RTs surfaced. |
| `render_target2d_bytes` / `render_target_cube_bytes` / `render_target2d_array_bytes` / `render_target_volume_bytes` | int64 | Per-subclass totals. |
| `entries` | array of `FBridgeRenderTargetEntry` | Top-N by `bytes` desc. |

### `FBridgeRenderTargetEntry`

| Field | Type | Notes |
|---|---|---|
| `path` | str | Asset path. |
| `type_name` | str | Class short name. |
| `width` / `height` / `depth` | int32 | Volume / cube / 2DArray use `depth` for slices/faces (1 for 2D, 6 for cube). |
| `bytes` | int64 | `GetResourceSizeBytes(EstimatedTotal)`. |

**Scope** — surfaces user-created + editor RTs only (`UTextureRenderTarget*` UObjects). **Does NOT cover** engine-internal RTs: GBuffer, shadow atlas, Lumen surface cache, virtual-texture pool, lighting cache. Those live inside the renderer module's private `FScene` state and require renderer-module access (out of scope for the bridge plugin). Use `stat scenerendertargets` or Insights for engine-internal RT memory.

---

## get_texture_streaming_residency(top_n=30) -> FBridgeTextureStreamingState

**(M7-1)** Top-N streaming textures by resident GPU bytes + global pool stats. Walks every UTexture2D via `TObjectIterator` on the GameThread; reads `GetStreamableResourceState()` for resident / wanted / max LOD counts and walks `PlatformData->Mips[*].BulkData` for cumulative byte sizes. Pool stats come from `IRenderAssetStreamingManager`.

| Top-level field | Type | Notes |
|---|---|---|
| `enabled` | bool | Project-wide streaming enabled (`r.TextureStreaming`). |
| `pool_size_bytes` | int64 | Streaming-pool budget. |
| `required_pool_bytes` | int64 | Bytes the streamer would consume with no pool limit. |
| `memory_over_budget_bytes` | int64 | Positive = streamer over budget. |
| `max_ever_required_bytes` | int64 | Peak required since last reset. |
| `num_streaming_textures` | int32 | Total streaming UTexture2D walked. |
| `rows` | array of `FBridgeTextureStreamingRow` | Top-N by `resident_bytes` desc. |

### `FBridgeTextureStreamingRow`

| Field | Type | Notes |
|---|---|---|
| `texture_path` | str | Asset path (e.g. `"/Game/.../T_Foo"`). |
| `resident_mip_count` / `wanted_mip_count` / `max_mip_count` | int32 | LOD counts. `resident < wanted` indicates not-yet-streamed-in. |
| `resident_bytes` / `wanted_bytes` | int64 | Cumulative mip bytes. Computed from `PlatformData->Mips[*].BulkData.GetBulkDataSize()`; falls back to `GetResourceSizeBytes(EstimatedTotal)` for textures whose data lives in DDC only. |
| `last_visible_seconds` | float | `app_time - GetLastRenderTimeForStreaming`. `FLT_MAX` = always-resident. |
| `force_resident` | bool | True when force-resident is set (cinematic / manual override). |

**Cost** — 5-50 ms depending on UTexture2D population. GameThread-only.

**Pitfalls**
- In the editor, BulkData for streamed mips often lives in DDC and the in-memory size is small. The fallback to `GetResourceSizeBytes` covers the common case but the absolute byte numbers are most accurate in cooked builds.
- `last_visible_seconds = FLT_MAX` means the texture isn't tracked by the streamer (always-resident or non-streamable), not "never visible".

---

## begin_insights_for_trace(utrace_path) -> FBridgeInsightsLaunchResult

**(M8-3)** Launch UnrealInsights.exe on a `.utrace` file. Detached — the call returns as soon as the process is created; Insights runs in its own window. Use when `parse_*_trace_to_summary` isn't enough and a human needs to take over with the visual timeline.

| Field | Type | Notes |
|---|---|---|
| `success` | bool | True when CreateProc succeeded. |
| `insights_exe_path` | str | Path to the launched UnrealInsights.exe. |
| `trace_path` | str | Echoed back. |
| `process_id` | int64 | OS process id of the launched Insights instance. |
| `error` | str | Failure diagnostic. |

**Path resolution** — Insights is at `<EngineRoot>/Engine/Binaries/Win64/UnrealInsights.exe`. Resolved via `FPaths::EngineDir()`.

---

## compare_perf_snapshots(before, after, regression_threshold=0.10) -> FBridgePerfSnapshotDelta

**(M8-2)** Diff two `FBridgePerfSnapshot` instances. Returns per-field deltas (`After - Before`) plus a list of human-readable regression strings for any metric that changed by ≥ `regression_threshold` (default 10%).

| Field | Type | Notes |
|---|---|---|
| `before_time_utc` / `after_time_utc` | str | Capture timestamps echoed back. |
| `delta_fps` / `delta_frame_ms` / `delta_game_thread_ms` / `delta_render_thread_ms` / `delta_gpu_ms` | float | Timing deltas (positive = slower). |
| `delta_draw_calls` / `delta_primitives_drawn` | int32 | Render counter deltas. |
| `delta_used_physical_mb` / `delta_used_virtual_mb` / `delta_peak_used_physical_mb` / `delta_available_physical_mb` | int64 | Memory deltas. |
| `delta_total_objects` / `delta_unique_classes` | int32 | UObject deltas (require `include_uobject_stats=True` on the snapshots). |
| `significant_regression` | bool | True when ≥ 1 regression exceeded threshold. |
| `regressions` | array of str | Pretty entries like `"frame_ms +25.0% (12.00 → 15.00 ms)"`. |

**Cost** — < 1 ms.

**Pitfalls**
- A metric with a 0 baseline can't compute a percentage and is skipped (no division by zero).
- `fps` regression is detected when `after < before` (lower = worse); other metrics flag when `after > before` (higher = worse).

**Example — pre/post-edit regression check**
```python
import unreal
before = unreal.UnrealBridgePerfLibrary.get_perf_snapshot(True)
# ... do edit / re-cook / etc ...
after  = unreal.UnrealBridgePerfLibrary.get_perf_snapshot(True)
delta  = unreal.UnrealBridgePerfLibrary.compare_perf_snapshots(before, after, 0.10)
if delta.significant_regression:
    print("REGRESSION:")
    for r in delta.regressions:
        print(f"  {r}")
```

---

## get_frame_time_percentiles(percentiles) -> array of float

**(M5-4)** Compute percentile frame times from the always-on internal frame-time histogram. The histogram is populated by an `OnEndFrame` hook that has been recording every frame since module load (see `reset_frame_time_histogram` to baseline before a measurement window).

| Param | Type | Notes |
|---|---|---|
| `percentiles` | array of float | One value per percentile to compute, each in `[0, 100]` (clamped). E.g. `[50, 90, 95, 99]`. Empty input returns empty output. |

Returns one ms value per requested percentile in the same order. Each percentile p resolves to the smallest frame time T such that ≥ p% of observed frames had total time ≤ T. Sub-bucket resolution is linear within the 0.5 ms internal bucket. The overflow bucket (frames > 200 ms) reports its lower edge — exact ms is unknown for those frames.

**Cost** — O(buckets + len(percentiles)) ≈ microseconds. Safe to poll.

**Example**
```python
# After warming up by running PIE / camera-flying for a while:
ps = unreal.UnrealBridgePerfLibrary.get_frame_time_percentiles([50, 90, 95, 99])
print(f"p50={ps[0]:.1f} ms  p90={ps[1]:.1f} ms  p95={ps[2]:.1f} ms  p99={ps[3]:.1f} ms")
```

**Pitfalls**
- Returns all zeros when no frames have been observed yet.
- For "before/after" comparisons, call `reset_frame_time_histogram()` before each measurement window — otherwise older frames leak in.
- p99 is the standard "worst real-world frame" metric. Looking at `frame_max_ms` is misleading — it picks up one-frame outliers like a ⟂ reload spike.

---

## parse_trace_to_summary(utrace_path, top_n=20, top_n_per_thread=10) -> FBridgePerfTraceSummary

Parse a `.utrace` file (output of `start_trace_capture` / `stop_trace_capture` or `Trace.Start File=…` console command) into a structured summary. Wraps `TraceServices::IAnalysisService::Analyze` synchronously, then walks the diagnostics + frame + timing-profiler + thread providers.

### Top-level fields

| Field | Type | Notes |
|---|---|---|
| `trace_path` | str | Echoed back. |
| `file_size_bytes` | int64 | OS-reported size of the `.utrace` file. |
| `platform` / `app_name` / `project_name` / `build_version` | str | From the `IDiagnosticsProvider`. |
| `changelist` | int32 | 0 if unknown. |
| `total_duration_seconds` | double | Sum of all valid Game frame durations. |
| `game_frame_count` | int32 | Number of valid Game frames. |
| `frame_avg_ms` / `frame_min_ms` / `frame_max_ms` | float | Per-frame stats over Game frames only. |
| `hot_scopes` | array of `FBridgePerfHotScope` | Top-N CPU timers across **all** CPU thread timelines, ranked by total inclusive time desc. |
| `gpu_hot_scopes` | array of `FBridgePerfHotScope` | **(M5-1)** Top-N GPU timers across every GPU queue (incl. legacy GPU1/GPU2 timelines for old traces). Empty when the trace did not include the `gpu` channel. |
| `per_thread_hot_scopes` | array of `FBridgePerThreadHotScopes` | **(M5-2)** One row per CPU thread (`GameThread`, `RenderThread N`, `RHIThread`, every `WorkerThread`, `FAssetDataGatherer`, …). Sorted by `total_cpu_ms` desc. |
| `load_time_breakdown` | array of `FBridgePerfLoadTimeRow` | **(M5-5)** Top-N packages by total load time (main thread + async loading thread). Empty when the trace lacks the `loadtime` channel **or** was captured after engine init when most packages were already cached. For useful cold-load attribution, capture from engine startup (`-trace=loadtime,frame,cpu` on the editor command line). |
| `counters` | array of `FBridgePerfCounter` | **(M5-3)** Trace counters with min/max/avg/last/sum aggregates over the full session interval. Ranked by `sample_count` desc — busiest counters surface first. Capped by `top_n_counters`. Counters with 0 samples are dropped. Empty when the trace lacks the `counter` channel. |
| `success` | bool | True on full success; on any failure the call returns with `success=False` + populated `error`. |
| `error` | str | Human-readable failure reason. |

### `FBridgePerfHotScope` row

| Field | Type | Notes |
|---|---|---|
| `name` | str | Timer name as registered (e.g. `RenderGraphExecute`, `FEngineLoop::Tick`). Bit-inverted metadata-tagged events are resolved back via `GetOriginalTimerIdFromMetadata`, so events with per-instance metadata collapse onto their canonical timer. |
| `total_ms` | double | Sum of inclusive time across every invocation. |
| `call_count` | int32 | Number of times the scope opened during the trace. |

### `FBridgePerfCounter` row

| Field | Type | Notes |
|---|---|---|
| `name` | str | Counter name (e.g. `"STAT_UnitGPU"`, `"FrameTime"`). |
| `group` | str | Group ID (e.g. `"STATGROUP_Engine"`, empty for ad-hoc TRACE_*VALUE). |
| `description` | str | Engine-supplied description. May be empty. |
| `floating_point` | bool | True for `TRACE_FLOAT_VALUE` / float stats; false for int. |
| `reset_every_frame` | bool | True for stats-style counters reset to 0 each frame. |
| `sample_count` | int32 | Number of samples in the trace. |
| `min_value` / `max_value` / `average_value` / `last_value` / `sum_value` | double | Aggregates over the full session. `last_value` is the most-recent sample. |

### `FBridgePerfLoadTimeRow` row

| Field | Type | Notes |
|---|---|---|
| `package_name` | str | E.g. `"/Game/Maps/Forest"`. |
| `total_loading_ms` | double | `main_thread_ms + async_loading_thread_ms`. |
| `main_thread_ms` | double | Time spent loading on the main thread. |
| `async_loading_thread_ms` | double | Time on the async loading thread (a.k.a. ALT). |
| `serialized_size_bytes` | int64 | `TotalSerializedSize` reported by the loadtime tracer. |
| `header_size_bytes` | int64 | `SerializedHeaderSize`. |
| `exports_size_bytes` | int64 | `SerializedExportsSize`. |
| `export_count` | int32 | Number of exports recorded under this package. |

### `FBridgePerThreadHotScopes` row

| Field | Type | Notes |
|---|---|---|
| `thread_name` | str | OS thread name (e.g. `"GameThread"`, `"RenderThread 0"`, `"Background Worker #24"`). |
| `group_name` | str | Engine thread group (`"Render"`, `"Background Workers"`, …). Empty for unknown. |
| `thread_id` | int64 | OS thread id. |
| `total_cpu_ms` | double | Sum of every scope's inclusive time on this thread. |
| `top_scopes` | array of `FBridgePerfHotScope` | Top-N scopes on this thread (capped at `top_n_per_thread`). |

### Parameters

- `utrace_path` (str): absolute path to a `.utrace` file. File must exist.
- `top_n` (int32): cap on `hot_scopes` and `gpu_hot_scopes` rows (clamped to `[1, 1000]`).
- `top_n_per_thread` (int32): cap on `top_scopes` per `per_thread_hot_scopes` row (clamped to `[1, 200]`). Pass **0 to skip the per-thread aggregation entirely** — saves time + memory on huge traces when only the global picture matters.
- `top_n_counters` (int32): cap on `counters` rows (clamped to `[1, 2000]`). Pass **0 to skip the counter walk entirely**.

### Cost

Dominated by `Analyze()` — typically 1-10s per 100 MB of trace. Synchronous, blocks the bridge exec; for large traces, expect tens of seconds (see `feedback_bridge_exec_holds_gamethread`).

### Channel requirements

| Want | Need channel |
|---|---|
| Frame stats | `frame` |
| `hot_scopes` + `per_thread_hot_scopes` | `cpu` |
| `gpu_hot_scopes` | `gpu` |
| `load_time_breakdown` | `loadtime` (typically + capture from engine startup) |

### Example

```python
import unreal
s = unreal.UnrealBridgePerfLibrary.parse_trace_to_summary(
    r"D:\Captures\my_session.utrace", top_n=20, top_n_per_thread=10)

print(f"frames={s.game_frame_count}  avg={s.frame_avg_ms:.2f}ms  worst={s.frame_max_ms:.0f}ms")

print("== top GPU hot scopes ==")
for r in s.gpu_hot_scopes:
    print(f"  {r.name:50s} {r.total_ms:8.1f} ms ({r.call_count})")

print("== thread breakdown ==")
for t in s.per_thread_hot_scopes[:5]:
    print(f"{t.thread_name} ({t.group_name})  total={t.total_cpu_ms:.0f} ms")
    for r in t.top_scopes[:3]:
        print(f"   - {r.name:48s} {r.total_ms:7.1f} ms")
```

### Pitfalls

- The trace must include the relevant channels at capture time. `gpu_hot_scopes` empty? Re-capture with `gpu` in the channel list.
- Events with metadata (e.g. `SlateUI Title = %s`) are collapsed to their canonical timer; per-instance breakdown is not reported.
- `total_cpu_ms` aggregates *inclusive* time per scope — adding rows across threads will overcount nested scopes.

---

## Cookbook

### Compare two points in time
```python
import unreal, time
before = unreal.UnrealBridgePerfLibrary.get_perf_snapshot(True)
# ... do some work ...
after = unreal.UnrealBridgePerfLibrary.get_perf_snapshot(True)

delta_mem_mb = after.memory.used_physical_mb - before.memory.used_physical_mb
delta_objs   = after.u_objects.total_objects   - before.u_objects.total_objects
print(f"Δmem = {delta_mem_mb:+d} MiB   Δobjs = {delta_objs:+d}")
```

### Watch the editor for 10 seconds
Run two bridge execs — the first starts a viewport camera flight / PIE / etc., the second samples. Bridge `exec` holds the GameThread, so *don't* loop sleep-and-sample inside a single exec (see `feedback_bridge_exec_holds_gamethread` in memory).

```python
# exec 1: kick off the thing you want to measure (PIE start / cinematic / etc.)
# exec 2:
import unreal, time, json
samples = []
end = time.time() + 10.0
while time.time() < end:
    t = unreal.UnrealBridgePerfLibrary.get_frame_timing()
    samples.append({"frame": t.frame_number, "fps": t.fps, "gt_ms": t.game_thread_ms, "rt_ms": t.render_thread_ms, "gpu_ms": t.gpu_ms})
    time.sleep(0.1)
print(json.dumps(samples))
```

Note: the loop runs on the GameThread (blocking). During the 10s sample window editor ticking is blocked — use short windows, or set up a reactive handler instead for longer captures.
