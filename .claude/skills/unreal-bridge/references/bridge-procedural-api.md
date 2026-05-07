# bridge-procedural-api

`unreal.UnrealBridgeProceduralLibrary` — procedural placement primitives (sampling / instancing) for level dressing. Call from Python to compose scatter pipelines without touching PCG graphs. See `docs/plans/procedural-content-roadmap.md` for the full design contract.

Hard contract:
- All sampling functions are **deterministic** given `(params, seed)` — internal `FRandomStream(seed)`, never `FMath::Rand`.
- Operates on the **editor world** (level dressing must survive PIE exit). For PIE-time scatter, use generic line traces from `UnrealBridgeLevelLibrary` directly.
- Trace-based sampling uses `ECC_Visibility` + `bTraceComplex=true`, matching the existing trace family.
- Hard cap of **100,000 points** per call. Above that, return empty + log warning. The 100k+ scale is PCG's territory.

**P0 + P1 fully shipped (23/23 Lane 1 ops)**:
- **M1 sampling**: grid, poisson2d, poisson3d, on_surface, on_landscape, on_spline, in_volume, jitter_stratified, transforms_along_spline, jitter_transforms.
- **M2 filtering**: by_slope, by_min_distance, by_overlap, by_density_mask, inside_actor, by_landscape_layer, project_to_surface.
- **M3 instancing**: ensure_ism_actor, add_instances, remove_by_ids, update_by_ids, clear, rebuild_nav.

Lane 2 (Geometry Script wrap) is the next milestone — see `docs/plans/procedural-content-roadmap.md` §4.

---

## sample_points_grid(bounds, spacing, jitter_ratio, seed) -> list[Vector]

Regular grid + per-cell jitter. Output count is fixed by `bounds` and `spacing`; the only stochastic part is the in-cell offset.

| Param | Type | Notes |
|---|---|---|
| `bounds` | `FBox` | XY range defines cells; output `Z = (bounds.min.z + bounds.max.z)/2`. |
| `spacing` | float (cm) | Cell size. Must be > 0; <=0 returns empty. |
| `jitter_ratio` | float | Per-cell offset as fraction of spacing, clamped to `[0, 0.5]`. 0 = strict lattice; 0.5 = each point can land anywhere in its cell. |
| `seed` | int32 | `FRandomStream` seed. |

Returns: list of world-space `unreal.Vector`. Empty if the would-be count exceeds the 100k cap (logs a warning).

**Cost** — O(N). Pure Python-side compute, no scene queries.

**Example**
```python
import unreal as u
P = u.UnrealBridgeProceduralLibrary
bounds = u.Box.build_aabb(u.Vector(0,0,0), u.Vector(5000, 5000, 0))   # 100m x 100m
pts = P.sample_points_grid(bounds, spacing=200.0, jitter_ratio=0.25, seed=42)
print(len(pts))  # ~676 (26x26 cells incl. upper edge anchor)
```

**Pitfalls**
- Cell count includes the upper edge anchor: `floor(size/spacing) + 1` per axis. A 5000-cm bound with 200-cm spacing yields **26 cells**, not 25. Account for this in your seed budget.
- Output `Z` is bounds-mid, not surface-projected. Pipe through `project_points_to_surface` (M2-7, forthcoming) when you need ground contact.

---

## sample_points_on_surface(actor_label, count, seed, max_bounce_up) -> list[Vector]

Random points draped on the surface of `actor_label` via downward line trace. Output count <= `count` (misses are dropped).

| Param | Type | Notes |
|---|---|---|
| `actor_label` | str | Target actor's user-visible label or FName. |
| `count` | int32 | Number of trace **attempts**. Hits ≤ count. Capped at 100,000. |
| `seed` | int32 | `FRandomStream` seed. |
| `max_bounce_up` | float (cm) | Padding added above bounds top before tracing down. 5000 covers typical undulation; raise for very tall actors. |

Workflow: actor world bounds → uniform-random XY in bounds → trace from `(X, Y, Z+max_bounce_up)` down to `(X, Y, Z-max_bounce_up)` on `ECC_Visibility` with `bTraceComplex=true` → push impact points.

**Cost** — O(`count`) line traces on GameThread. Empirically ~5-10μs per trace on simple geometry; budget ~50ms for 5000 attempts. Above ~5k attempts, split across multiple `bridge.exec` calls (per `feedback_bridge_exec_holds_gamethread`).

**Example**
```python
pts = P.sample_points_on_surface("LandscapeActor", count=2000, seed=42, max_bounce_up=5000.0)
print(f"got {len(pts)} hits from 2000 attempts")
```

**Pitfalls**
- For Landscape specifically, this is 5-10x slower than the forthcoming `sample_points_on_landscape` (M1-5) which uses `ALandscapeProxy::GetHeightAtLocation`. Use this version when you need PIE compatibility or non-Landscape surfaces.
- Hit rate < 100%: misses (sky, gaps in terrain, holes) are silently dropped — caller can't distinguish "actor too small" from "all rays missed". Compare `len(pts)` to `count` to detect.
- Actor bounds includes child actors (`bIncludeFromChildActors=true`) — large vehicles or compound actors may have looser bounds than visual extent.

---

## sample_points_on_landscape(landscape_label, bounds_2d, count, seed) -> list[Vector]

Random points on a Landscape via direct height-query API (`ALandscapeProxy::GetHeightAtLocation`). 5-10x faster than `sample_points_on_surface` for landscape targets. Iterates root + all `LandscapeStreamingProxy` actors automatically — handles World Partition / Landscape Streaming sublevels transparently.

| Param | Type | Notes |
|---|---|---|
| `landscape_label` | str | Target Landscape actor's label or FName. Must `Cast<ALandscapeProxy>` cleanly. |
| `bounds_2d` | `FBox` | XY range to sample. Z component on input is ignored; output Z is the queried surface height. |
| `count` | int32 | Number of XY samples. Capped at 100,000. |
| `seed` | int32 | `FRandomStream` seed. |

Returns: list of points. Empty if landscape not found / Count > cap / no `LandscapeInfo` (typical when level isn't loaded).

**Cost** — O(count × proxy_count). For a single-proxy landscape ~200ns/point, vs ~5-10μs/point for line-trace based `sample_points_on_surface`.

**Example**
```python
bounds_xy = u.Box.build_aabb(u.Vector(0,0,0), u.Vector(10000, 10000, 0))
pts = P.sample_points_on_landscape("Landscape_0", bounds_xy, count=5000, seed=42)
print(f"{len(pts)} hits inside landscape coverage")
```

**Pitfalls**
- **Editor-only** — `ULandscapeInfo` is not populated in shipping builds and not always present in PIE. For PIE-time sampling use `sample_points_on_surface` (M1-4).
- A `LandscapeStreamingProxy` whose level is unloaded does NOT contribute heights — its slice of the landscape returns no hits and points there are silently dropped. Pre-load relevant streaming levels first via `unreal.UnrealBridgeLevelLibrary.get_streaming_levels()` if you need full coverage.
- `GetHeightAtLocation` defaults to `EHeightfieldSource::Complex` (heightmap mesh, exact). Bridge does not expose Simple-collision mode currently.

---

## sample_points_poisson_disk2d(bounds, min_radius, max_attempts, seed) -> list[Vector]

Bridson 2D Poisson-disk sampling — every output point ≥ `min_radius` from every other in XY plane. For natural-looking forest / scatter where regular grid spacing is visible.

| Param | Type | Notes |
|---|---|---|
| `bounds` | `FBox` | XY range. Z ignored on input; output Z = `bounds.Min.Z` (pipe through `project_points_to_surface` for ground contact). |
| `min_radius` | float (cm) | Minimum distance between any two points. > 0 required. |
| `max_attempts` | int32 | Per-active-point candidate retries. **30** is the canonical Bridson 2007 value; raising helps tight packings. ≤ 0 is silently treated as 30. |
| `seed` | int32 | `FRandomStream` seed. |

Returns: point set. Output count is **not** controllable directly — depends on bounds area, min_radius, and stochastic rejection. Capped at 100,000.

**Cost** — Worst-case O(N · max_attempts). Uses background grid (`cell = R/√2`) for O(1) neighbor lookup. For a 10m × 10m bounds with R=2m and max_attempts=30, expect ~25 points and ~100μs total.

Memory: `O((bounds_area / R²))` cells, each `int32`. Refused if grid would exceed 4 × cap (`400k cells`) — raise `min_radius` or shrink `bounds` if you hit this.

**Example**
```python
bounds = u.Box.build_aabb(u.Vector(0,0,0), u.Vector(10000, 10000, 0))
pts = P.sample_points_poisson_disk2d(bounds, min_radius=300.0, max_attempts=30, seed=42)
print(f"{len(pts)} naturally-spaced points, all ≥ 3m apart")
```

**Pitfalls**
- Output count varies with seed even at fixed params — Bridson's stochastic rejection means seed=42 might give 1247 points and seed=43 give 1239. Acceptable for most use cases; if you need exact count, oversample then `random.shuffle + truncate` on the Python side.
- For irregular regions (not axis-aligned boxes), sample inside the bounding rectangle then post-filter via `filter_points_inside_actor` (forthcoming M2-5).
- 5×5 neighborhood scan is correct for cell = R/√2 — tweaking cell size requires reanalyzing the conflict-detection radius.

---

## sample_points_poisson_disk3d(bounds, min_radius, max_attempts, seed) -> list[Vector]

3D Bridson — every output point ≥ `min_radius` apart in 3D. For volumetric scatter (asteroid fields, particle clouds, floating debris). `cell = R/√3`, 5×5×5 neighborhood scan, spherical-annulus candidate generation.

| Param | Type | Notes |
|---|---|---|
| `bounds` | `FBox` | 3D box. All three extents must be > 0. |
| `min_radius` | float (cm) | Min 3D distance between points. |
| `max_attempts` | int32 | Per-active-point retries. ≤ 0 → 30 (Bridson default). |
| `seed` | int32 | `FRandomStream` seed. |

Returns: point set, capped at 100,000.

**Cost** — Worst-case `O(N · max_attempts)`. Memory: `O(volume / R³)` cells; refused if grid > 4× the 100k cap.

**Example**
```python
b = u.Box(u.Vector(0,0,0), u.Vector(5000, 5000, 2000))
pts = P.sample_points_poisson_disk3d(b, min_radius=300.0, max_attempts=30, seed=42)
```

**Pitfalls**
- 3D output count varies more wildly with seed than 2D. For deterministic count, oversample + truncate on Python side.
- Memory cap applies to the **grid** (volume / R³), not output count — small `R` relative to bounds OOMs the grid first.

---

## sample_points_on_spline(spline_actor_label, component_name, mode, count_or_spacing) -> list[Vector]

Sample evenly-spaced positions along a `USplineComponent`. Output is in **world space**.

| Param | Type | Notes |
|---|---|---|
| `spline_actor_label` | str | Actor owning the spline. |
| `component_name` | str | Spline component name. **Empty string = first SplineComponent found.** |
| `mode` | str | `"by_count"` or `"by_distance"`. Bad value → empty list + log. |
| `count_or_spacing` | float | int(N) for `by_count`; cm spacing for `by_distance`. |

Returns: world-space points along the spline.

**Cost** — O(N) calls to `GetLocationAtDistanceAlongSpline`. Negligible.

**Example**
```python
# 50 lampposts evenly along a 200m road spline
pts = P.sample_points_on_spline("BP_RoadSpline_0", "", "by_count", 50.0)

# Or: every 5m, regardless of total length
pts = P.sample_points_on_spline("BP_RoadSpline_0", "", "by_distance", 500.0)
```

**Pitfalls**
- `by_count` includes both endpoints (N points = N-1 segments). For cyclical layouts, use `by_distance` or trim the last point.
- Spline must be loaded — World Partition / streaming may delay availability. Check via `unreal.UnrealBridgeLevelLibrary.get_actor_info(label)` first.

---

## sample_points_in_volume(volume_actor_label, count, seed, max_attempts) -> list[Vector]

Reject-sampled points inside a volume actor. For `AVolume` subclasses (`ATriggerVolume`, `ABlockingVolume`, etc) uses `EncompassesPoint` (accurate brush geometry). For non-volume actors falls back to actor world bounds (loose AABB).

| Param | Type | Notes |
|---|---|---|
| `volume_actor_label` | str | Volume actor. |
| `count` | int32 | Target output count (≤ 100k). |
| `seed` | int32 | `FRandomStream` seed. |
| `max_attempts` | int32 | Per-target reject-sample retries. ≤ 0 → 30. Total work cap = `count × max_attempts`. |

Returns: up to `count` points inside the volume.

**Cost** — `O(count × max_attempts)` at worst. For thin/long volumes, output may be far below `count` even at high attempts (rejection rate too high).

**Example**
```python
pts = P.sample_points_in_volume("BP_ForestZone", count=500, seed=42, max_attempts=30)
```

**Pitfalls**
- Non-`AVolume` actors fall back to bounds (AABB), which is loose — points may land outside the actual collision shape. Wrap your placement zone in a `BoxVolume` / `BlockingVolume` for accurate containment.
- Output count is NOT guaranteed to reach `count`. If the volume occupies < 30% of its bounds (typical for a thin trigger), expect lower hit rates. Compare `len(pts)` to `count` to detect.

---

## sample_points_jitter_stratified(bounds, grid_resolution, seed) -> list[Vector]

Per-cell jittered grid — the cheap O(N) alternative to Poisson when fully natural distribution isn't required. Each cell gets one random point inside it.

| Param | Type | Notes |
|---|---|---|
| `bounds` | `FBox` | XY bounds; output Z = `(min.z + max.z) / 2`. |
| `grid_resolution` | int32 | Cells per axis. Total points = `grid_resolution²`. Capped at 100k total. |
| `seed` | int32 | `FRandomStream` seed. |

Returns: exactly `grid_resolution²` points (no rejection).

**Cost** — O(N) constant work per cell. Instant for typical inputs.

**Example**
```python
# 50×50 grid of jittered points = 2500 spots
pts = P.sample_points_jitter_stratified(bounds, grid_resolution=50, seed=42)
```

**Pitfalls**
- Output is **fixed count**, unlike Poisson. Use this when you need a known target count and "natural-ish but not perfect" spacing.
- Two adjacent cells' points can be arbitrarily close at their shared edge — there's no min-distance guarantee. Add `filter_points_by_min_distance` if minimum spacing matters.

---

## sample_transforms_along_spline(spline_actor_label, component_name, mode, count_or_spacing) -> list[Transform]

Same as `sample_points_on_spline`, but yields full `FTransform`: location + rotation aligned to spline tangent. The standard "lampposts / fence posts / road markers" placement primitive.

Same params as `sample_points_on_spline`.

**Example**
```python
xs = P.sample_transforms_along_spline("BP_FenceSpline", "", "by_distance", 200.0)
# Each transform's rotation has yaw matching the spline tangent. Add scale via JitterTransforms.
ids = P.add_instances_by_transforms(actor, xs, world_space=True)
```

**Pitfalls**
- Roll is always 0; pitch comes from tangent's vertical component. For perfectly upright posts on hilly splines, post-process the transforms to zero out pitch.
- Tangent is forward-direction (X-axis after rotation). If your mesh forward is Y or Z, pre-rotate via `JitterTransforms`-like helper or modify your asset.

---

## jitter_transforms(xs, pos_sigma, rot_sigma, scale_min, scale_max, seed) -> list[Transform]

Post-process jitter on an existing transform list. Adds Gaussian noise to position (Box-Muller) and Euler rotation, plus uniform-random scale per-axis between `scale_min` and `scale_max`. Deterministic.

| Param | Type | Notes |
|---|---|---|
| `xs` | `list[Transform]` | Input transforms. |
| `pos_sigma` | float (cm) | Std dev of position Gaussian noise (per axis). |
| `rot_sigma` | float (deg) | Std dev of rotation Gaussian noise (per Euler axis). |
| `scale_min` | `Vector` | Per-axis lower scale multiplier. |
| `scale_max` | `Vector` | Per-axis upper scale multiplier. |
| `seed` | int32 | `FRandomStream` seed. |

Returns: jittered transforms, same length as input.

**Cost** — O(N), pure math. Instant.

**Example**
```python
base = [u.Transform(p, u.Rotator(0,0,0), u.Vector(1,1,1)) for p in pts]
jittered = P.jitter_transforms(base,
    pos_sigma=50.0,           # ±50cm Gaussian per axis
    rot_sigma=15.0,           # ±15° Gaussian per Euler axis
    scale_min=u.Vector(0.8, 0.8, 0.8),
    scale_max=u.Vector(1.3, 1.3, 1.3),
    seed=42)
```

**Pitfalls**
- Position noise is **3D** — if you only want XY jitter, follow with `project_points_to_surface` to re-snap Z.
- Scale is **multiplicative on input transforms** (not absolute). If input scale is 1.0 and `scale_min/max` is `(0.8, 1.2)`, output scale is in `[0.8, 1.2]`. If input scale is 2.0, output is in `[1.6, 2.4]`.

---

## filter_points_by_slope(in, max_slope_deg, bounce_up) -> list[Vector]

Drop points sitting on terrain steeper than `max_slope_deg`. For each input, traces down, takes impact normal, keeps point only when angle from +Z ≤ threshold.

| Param | Type | Notes |
|---|---|---|
| `in` | `list[Vector]` | Input point list. |
| `max_slope_deg` | float | Maximum slope angle in degrees, clamped to [0, 90]. 0 = flat only; 90 = no filter. |
| `bounce_up` | float (cm) | cm above each point to start trace; symmetric distance below as endpoint. 5000 typical. |

Returns: filtered list (≤ input length). Misses are dropped (no surface = no slope to evaluate).

**Cost** — O(N) line traces. ~5-10μs per trace.

**Example**
```python
# 30° max slope — typical "no trees on cliffs" rule
walkable = P.filter_points_by_slope(pts, max_slope_deg=30.0, bounce_up=5000.0)
print(f"kept {len(walkable)}/{len(pts)} below 30°")
```

**Pitfalls**
- Output is **original points**, not surface-projected. Pipe through `project_points_to_surface` (M2-7) afterwards if you also need ground snap.
- Distinguishes "below threshold" (kept) from "no surface" (dropped) only by absence — caller can't tell which case caused a drop. Use `project_points_to_surface` to inspect (which keeps misses with up-normal).
- The trace symmetric range is `±bounce_up`. If your points sit far off-surface, raise `bounce_up` — otherwise the trace ends before reaching the ground and you get false misses.

---

## filter_points_by_min_distance(in, min_dist) -> list[Vector]

Greedy first-come-wins thinning by XY distance. Order-preserving among kept points. The standard "post-Poisson second pass with a different scale" or "thin grid before instancing" tool.

| Param | Type | Notes |
|---|---|---|
| `in` | `list[Vector]` | Input points. |
| `min_dist` | float (cm) | Minimum XY distance to maintain between any two kept points. ≤ 0 = pass-through (no filter). |

Returns: filtered list. Order matches input (kept-only).

**Cost** — O(N) amortized via flat XY grid bucket (`cell = MinDist/√2`, 5×5 neighborhood). For 10k inputs at typical density, ~2-5ms total.

Memory: refused if grid would exceed 10× the 100k-cap (1M cells). Raise `min_dist` or shrink input span if hit.

**Example**
```python
# Sample dense grid then thin to ≥ 5m spacing
grid_pts = P.sample_points_grid(bounds, spacing=100.0, jitter_ratio=0.3, seed=42)
print(f"grid: {len(grid_pts)} points")
thinned = P.filter_points_by_min_distance(grid_pts, min_dist=500.0)
print(f"thinned: {len(thinned)} points (≥ 5m apart)")
```

**Pitfalls**
- Filter is **2D distance** (XY plane only) — matches Poisson's 2D model. For 3D distance use case, no 3D filter exists yet (out of P0).
- "First-come-wins" depends on input order. Permuting the input may yield a different (still valid) output set with different points kept.
- Does not project — input Z passes through unchanged. Pair with `project_points_to_surface` (before or after) for ground contact.

---

## filter_points_by_overlap(pts, blocking_class_paths, radius) -> list[Vector]

Drop points whose sphere-overlap (radius `radius` cm) hits any actor of any class in `blocking_class_paths`. The "avoid roads / buildings / water" filter.

| Param | Type | Notes |
|---|---|---|
| `pts` | `list[Vector]` | Input points. |
| `blocking_class_paths` | `list[str]` | Class paths (`/Game/BP/BP_Building.BP_Building` or short names). Resolved at start; bad entries silently skipped. `_C` suffix appended automatically when needed. |
| `radius` | float (cm) | Sphere overlap test radius. > 0 required; ≤ 0 = pass-through. |

Returns: filtered list — points NOT overlapping any blocking actor.

**Cost** — O(N) sphere overlap queries on GameThread. Each query is fast (a few μs for typical scenes). Above ~10k points split across `bridge.exec` calls.

**Example**
```python
# Drop tree candidates within 5m of any building or road
clear = P.filter_points_by_overlap(pts, ["/Game/BP/BP_Building.BP_Building",
                                          "/Game/BP/BP_Road.BP_Road"], radius=500.0)
```

**Pitfalls**
- If all class paths fail to resolve, the function falls through to **pass-through** (returns input unchanged) rather than dropping everything. Check log for "failed to load" if surprised by zero filtering.
- Class paths to BP assets may need `_C` suffix to resolve as the generated class. The function tries both, but verify your paths point to assets with collision configured.
- `bTraceComplex=false` for sphere overlaps — uses simple collision, faster but may miss thin geometry (paper-thin walls). For accuracy on complex meshes, compose with bounds-based filters first.

---

## filter_points_by_density_mask(pts, texture_asset, bounds_xy, channel_index, threshold, seed) -> list[Vector]

Texture-driven density modulation. Maps each point's XY into the texture via `bounds_xy → UV`, samples the requested channel of mip 0, and keeps the point with stochastic probability based on the texel value.

Selection rule:
1. Sample channel value `c ∈ [0, 1]`.
2. If `c < threshold` → drop (hard cutoff; `threshold=0` means no cutoff).
3. Else keep with probability `c` (stochastic — high mask values pass more).

| Param | Type | Notes |
|---|---|---|
| `pts` | `list[Vector]` | Input points. |
| `texture_asset` | str | `/Game/...` content path of `UTexture2D`. |
| `bounds_xy` | `FBox` | World-space XY range that maps to texture's [0,1] UV. Z ignored. |
| `channel_index` | int32 | 0=R, 1=G, 2=B, 3=A. Out-of-range falls back to R. |
| `threshold` | float | Hard cutoff [0, 1]. 0 = pure stochastic; 0.5 = require mask ≥ 0.5 first. |
| `seed` | int32 | `FRandomStream` seed. |

Returns: filtered list (≤ input length).

**Cost** — O(N) texel lookups (single L1 hit per point). Memory: locks mip 0 once, unlocks at end.

**Example**
```python
# Use a forest density mask R channel; require ≥ 30% paint, then probabilistic
forest = P.filter_points_by_density_mask(pts,
    texture_asset="/Game/Masks/T_ForestDensity",
    bounds_xy=bounds, channel_index=0, threshold=0.3, seed=42)
```

**Pitfalls**
- **Critical** — texture must have **`CompressionSettings = TC_VectorDisplacementmap`**. Default compression (DXT/BC) is GPU-only; the function returns empty + log warning otherwise. Set this in the texture asset properties and reimport. Plan §6 #11.
- Points outside `bounds_xy` are dropped (no UV → no sample). Make `bounds_xy` slightly larger than your sampling region if you want edge points to all get evaluated.
- `threshold` is a hard cutoff, NOT a "minimum density". Value 0.5 doesn't mean "50% density" — it means "any texel value < 0.5 is rejected outright before stochastic step".

---

## filter_points_inside_actor(pts, container_actor_label, b_inside) -> list[Vector]

Keep or drop points based on whether they're inside `container_actor_label`'s collision shape. For `AVolume` and subclasses uses `EncompassesPoint` (accurate brush). For non-volume actors falls back to AABB containment (loose).

| Param | Type | Notes |
|---|---|---|
| `pts` | `list[Vector]` | Input points. |
| `container_actor_label` | str | Actor to test against. |
| `inside` | bool | `True` = keep points inside; `False` = keep outside. |

Returns: filtered list (≤ input length).

**Cost** — O(N) point-in-volume tests. AVolume's `EncompassesPoint` is fast; AABB fallback is O(1).

**Example**
```python
# Keep only points inside a "PlayableArea" trigger volume
playable = P.filter_points_inside_actor(pts, "BP_PlayableArea", inside=True)

# Or: drop points inside a "NoSpawnZone"
safe = P.filter_points_inside_actor(pts, "BP_NoSpawnZone", inside=False)
```

**Pitfalls**
- Non-volume actors fall back to bounds — sphere-shaped or thin actors give very loose results. Use `AVolume` subclasses (`ATriggerVolume`, `ABlockingVolume`) for precise containment.
- bool kwarg is `inside`, not `b_inside` (b-prefix stripped per UE Python convention).

---

## filter_points_by_landscape_layer(pts, landscape_label, layer_name, threshold) -> list[Vector]

Drop points where the named landscape weightmap layer has weight below `threshold`. Iterates landscape components and uses `EditorGetPaintLayerWeightByNameAtLocation` per point.

| Param | Type | Notes |
|---|---|---|
| `pts` | `list[Vector]` | Input points. |
| `landscape_label` | str | `ALandscape` / `ALandscapeProxy` actor label. |
| `layer_name` | `Name` (FName) | Paint layer name, e.g. `"Grass"`, `"Forest"`. Must match a layer in the landscape's paint layer list. |
| `threshold` | float | Minimum weight `[0, 1]` to keep. 0 = pass-through; 1 = layer must be fully painted. |

Returns: filtered list. Points outside landscape coverage are dropped.

**Cost** — Editor-only. O(P × C) where C = landscape component count (each point iterates all components to find its containing one, then samples). For 10k points on a 16-component landscape, ~30-100ms. Above this scale, split exec.

**Example**
```python
forest_pts = P.filter_points_by_landscape_layer(pts,
    landscape_label="Landscape_0",
    layer_name=u.Name("Forest"),
    threshold=0.5)
```

**Pitfalls**
- **Editor-only** — `EditorGetPaintLayerWeightByNameAtLocation` doesn't exist in shipping. For PIE-time use, no equivalent yet — bake your decisions into a density mask texture (M2-4) instead.
- If `layer_name` doesn't exist on the landscape, weight is 0 for all points → all dropped. Verify name via `Landscape Editor → Paint → Target Layers` in the editor.
- Linear search through components per point — fine at the 100k cap, sluggish above.

---

## project_points_to_surface(in, bounce_up, bounce_down, out_hit_normals) -> list[Vector]

Project each input point vertically onto whatever's beneath (or above, within `bounce_up`) via line trace. The standard "after Poisson2D, drape onto terrain" finishing step. Output array is **always parallel** to input — same length, no filtering.

| Param | Type | Notes |
|---|---|---|
| `in` | `list[Vector]` | Input point list. |
| `bounce_up` | float (cm) | cm above each input Z to start the trace. 5000 typical. |
| `bounce_down` | float (cm) | cm below each input Z as trace endpoint. 5000 typical. |
| `out_hit_normals` | [out] `list[Vector]` | Parallel array of impact normals. `(0,0,1)` on miss. |

Returns: projected points. **Same length as input.** Misses pass the original point through unchanged with a +Z normal.

In Python, the `out_*` parameter pattern returns a tuple: `(projected_pts, normals) = P.project_points_to_surface(in, ...)`.

**Cost** — O(N) line traces on GameThread. ~5-10μs per trace; budget ~5ms per 1000 points. Above ~10k inputs split across multiple `bridge.exec` calls.

**Example**
```python
poisson_pts = P.sample_points_poisson_disk2d(bounds, 300.0, 30, 42)
projected, normals = P.project_points_to_surface(poisson_pts, 5000.0, 5000.0)
# Now `projected` has correct ground Z, `normals` has surface orientation per point.
# Build transforms with normal-aligned rotation:
xs = []
for pt, n in zip(projected, normals):
    rot = u.Rotator(0,0,0).from_axis_and_angle(...)  # depends on use-case
    xs.append(u.Transform(pt, rot, u.Vector(1,1,1)))
```

**Pitfalls**
- Misses produce a degenerate (original_point, up_normal) entry — does NOT mark them. If you need to drop misses, post-filter on Python side by checking which output points moved vs original.
- Trace channel is `ECC_Visibility` with complex collision, matching the rest of the trace family. Glass / water / collision-disabled actors may pass through.
- Editor-only: requires editor world. Won't run inside PIE the same way.

---

## ensure_procedural_ism_actor(tag, mesh_path, use_hism) -> str

Find or create a stub `AActor` carrying a single `UInstancedStaticMeshComponent` (or `UHierarchicalInstancedStaticMeshComponent` if `use_hism`) configured to render `mesh_path`. Idempotent — calling twice with the same `tag` returns the same actor.

| Param | Type | Notes |
|---|---|---|
| `tag` | str | Unique grouping key. The created actor carries this as `FName` in its `Tags` list. |
| `mesh_path` | str | `/Game/...` content path of the StaticMesh asset. |
| `use_hism` | bool | `True` for HISM (with culling/LOD; preferred for foliage-density scatter). `False` for plain ISM (no hierarchy). |

Returns: actor label (e.g. `"Procedural_Trees_PineForest"`), or empty string on failure.

**Cost** — O(1) on reuse, single `SpawnActor` + `NewObject` + `RegisterComponent` on first call. ~1-2ms.

**Example**
```python
actor = P.ensure_procedural_ism_actor("Trees_PineForest",
    "/Game/Trees/SM_Pine", use_hism=True)
# Subsequent call returns the same actor — idempotent.
same = P.ensure_procedural_ism_actor("Trees_PineForest", "/Game/Trees/SM_Pine", True)
assert actor == same
```

**Pitfalls**
- **Tag-based lookup is mesh-blind**: if an actor already exists with `tag`, this function returns it *regardless* of whether its current mesh matches `mesh_path`. To swap the mesh on an existing stub, destroy the actor first via `unreal.UnrealBridgeLevelLibrary.destroy_actor(label)` then re-call this function.
- Spawned class is `AActor`, not `AStaticMeshActor` — `AStaticMeshActor`'s `RootComponent` is a plain `UStaticMeshComponent` and cannot be swapped to ISM. The stub looks empty in the World Outliner's class column ("Actor") — that's intentional.
- `FScopedTransaction` is wrapped, so Ctrl+Z undoes the actor creation.

---

## add_instances_by_transforms(actor_name, xs, world_space) -> list[int32]

Bulk-add instances to a procedural ISM actor's `[H]ISMC` in one call. Returns the instance index of each transform in the order added — required for later `update`/`remove` ops.

| Param | Type | Notes |
|---|---|---|
| `actor_name` | str | Label returned by `ensure_procedural_ism_actor`. |
| `xs` | `list[Transform]` | Instance transforms. |
| `world_space` | bool | `True` = `xs` are world-space; component handles the inverse-transform internally. `False` = local. Pick whichever your sampling pipeline produced. |

Returns: instance ID list, parallel to `xs`. Empty on actor-not-found / no-ISM / empty input.

**Cost** — One `AddInstances` call, internally batched. ~1ms per 1000 instances on HISM.

**Example**
```python
xs = [u.Transform(p, u.Rotator(0,0,0), u.Vector(1,1,1)) for p in pts]
ids = P.add_instances_by_transforms("Procedural_Trees_PineForest", xs, world_space=True)
print(f"added {len(ids)} instances, first id={ids[0]}")
```

**Pitfalls**
- `b_should_return_indices=true` is forced internally — without it the Engine API silently returns no IDs (perf optimisation), and you lose the ability to update/remove individual instances later.
- `b_update_navigation=false` is forced — call the forthcoming `rebuild_procedural_navigation` (M3-6) once at end-of-batch instead of N times mid-batch. This cuts nav rebuild cost from O(N) to O(1).
- Instance IDs are stable across `add` / `remove` cycles only if you remove with `remove_instances_by_ids` (M3-3, forthcoming). Direct `ClearInstances` resets the ID space.

---

## rebuild_procedural_navigation(actor_name) -> bool

Single end-of-batch nav rebuild for a procedural ISM actor. Pairs with `add_instances_by_transforms` (which defers nav update with `bUpdateNavigation=false`) — call this once after the batch instead of N times during it.

| Param | Type | Notes |
|---|---|---|
| `actor_name` | str | Label returned by `ensure_procedural_ism_actor`. |

Returns: `True` iff actor + ISMC resolved.

Two effects:
1. For HISM, `BuildTreeIfOutdated(false, true)` synchronously rebuilds the cluster tree (used by both rendering and nav-relevance queries). For plain ISM this is a no-op.
2. `FNavigationSystem::UpdateComponentData` re-registers the component with the nav octree, mirroring `AddInstances(bUpdateNavigation=true)`'s internal behavior.

**Cost** — Sync HISM tree rebuild is O(N log N) in instance count; nav octree update is amortized O(component bounds area). For 10k instances expect ~50-200ms — well worth it vs. 10k × per-call rebuilds.

**Example**
```python
# After a batch of add_instances_by_transforms calls
for tile in forest_tiles:
    P.add_instances_by_transforms("Forest_Pines", tile.xforms, True)
P.rebuild_procedural_navigation("Forest_Pines")  # ← one nav rebuild
```

**Pitfalls**
- Skipping this after a batch leaves AI pathfinding using stale nav — agents may walk through your trees. Always call after a batch.
- Async `BuildTreeIfOutdated` is also valid (`Async=true`) and faster, but not exposed here — the caller usually wants a deterministic sync rebuild before testing nav (e.g. running an AI test immediately after).
- Only rebuilds nav for the one actor's component. If multiple procedural actors changed, call once per actor.

---

## remove_instances_by_ids(actor_name, instance_ids) -> bool

Bulk-remove instances by ID. Sorts the IDs in reverse and dedupes before calling `RemoveInstances` with the `bAlreadySortedReverse=true` flag — the canonical UE pattern that avoids per-removal swap-back errors. Negative IDs silently dropped.

| Param | Type | Notes |
|---|---|---|
| `actor_name` | str | Procedural ISM actor label. |
| `instance_ids` | `list[int32]` | IDs to remove (returned by `add_instances_by_transforms`). |

Returns: `True` iff actor + ISM resolved.

**Cost** — O(N log N) sort + O(N) dedup, then single `RemoveInstances` call. Wrapped in `FScopedTransaction` for undo.

**Example**
```python
ids = P.add_instances_by_transforms(actor, xs, world_space=True)
# ... later, remove a subset
P.remove_instances_by_ids(actor, ids[:50])  # remove first 50
# IDs of REMAINING instances may have shifted (UE swap-back); call clear+re-add for predictable IDs
```

**Pitfalls**
- After `remove_instances_by_ids`, the surviving instances' IDs may be remapped by UE's swap-back removal. If you need stable IDs across edits, call `clear_instances` and re-add the desired set instead.
- Negative IDs are dropped silently. Out-of-range positive IDs go to UE which logs a warning.

---

## update_instance_transforms_by_ids(actor_name, ids, new_xs, world_space) -> bool

Bulk-update instance transforms by ID. Calls `UpdateInstanceTransform` per pair with `bMarkRenderStateDirty=false`, then a single `MarkRenderStateDirty()` at the end — much cheaper than N per-call dirties.

| Param | Type | Notes |
|---|---|---|
| `actor_name` | str | Procedural ISM actor label. |
| `ids` | `list[int32]` | Instance IDs to update. Must equal length of `new_xs`. |
| `new_xs` | `list[Transform]` | New transforms, parallel to `ids`. |
| `world_space` | bool | `True` if `new_xs` are world-space; `False` for local. |

Returns: `True` iff lengths match + actor + ISM resolved.

**Cost** — O(N) per-instance update + 1 render-state-dirty. ~1ms per 1000 instances.

**Example**
```python
# After scattering, sway every other tree slightly
sway_ids = ids[::2]
sway_xs = [u.Transform(t.translation, u.Rotator(0, t.rotation.yaw + 5.0, 0), t.scale)
           for t in original_xs[::2]]
P.update_instance_transforms_by_ids(actor, sway_ids, sway_xs, world_space=True)
```

**Pitfalls**
- Lengths must match exactly. Mismatch = early return + log + no-op.
- IDs that don't exist on the component get a UE warning per call but don't fail the whole batch.
- For massive transforms updates over an entire HISM (every instance), `clear_instances` + `add_instances_by_transforms` is often faster (single batch vs N per-instance updates).

---

## clear_instances(actor_name) -> bool

Remove every instance from the actor's `[H]ISMC`. The actor itself remains, ready for the next batch. Wrapped in `FScopedTransaction` so undo restores the previous instance set.

| Param | Type | Notes |
|---|---|---|
| `actor_name` | str | Label returned by `ensure_procedural_ism_actor`. |

Returns: `True` iff actor + ISM resolved and `ClearInstances` was called.

**Cost** — O(1). Single `ClearInstances` + `MarkRenderStateDirty`.

**Example**
```python
P.clear_instances("Procedural_Trees_PineForest")
# Now safe to re-seed:
ids = P.add_instances_by_transforms("Procedural_Trees_PineForest", new_xs, True)
```

**Pitfalls**
- Doesn't destroy the stub actor. To fully remove the procedural group, `clear_instances` then `unreal.UnrealBridgeLevelLibrary.destroy_actor(label)`.
- Instance ID space resets to 0 — old IDs returned by `add_instances_by_transforms` become invalid. Save them before clearing if you need cross-batch correlation.

---

## End-to-end recipe (P0 + P1 complete, 23 ops)

A naturalistic forest scatter on a Landscape, slope/road/density/layer filtered, jittered transforms, two-tier hero+filler with stable nav:

```python
import unreal as u
P = u.UnrealBridgeProceduralLibrary

bounds = u.Box(u.Vector(0, 0, 0), u.Vector(20000, 20000, 0))   # 200m × 200m

# 1) Sample with natural spacing, then drape onto landscape
poisson = P.sample_points_poisson_disk2d(bounds, min_radius=500.0, max_attempts=30, seed=42)
projected, normals = P.project_points_to_surface(poisson, 5000.0, 5000.0)

# 2) Multi-pass filter chain
walkable    = P.filter_points_by_slope(projected, max_slope_deg=30.0, bounce_up=5000.0)
forest_zone = P.filter_points_by_landscape_layer(walkable, "Landscape_0",
                                                  u.Name("Forest"), threshold=0.3)
clear       = P.filter_points_by_overlap(forest_zone,
                                          ["/Game/BP/BP_Road.BP_Road"], radius=500.0)
modulated   = P.filter_points_by_density_mask(clear,
                                                "/Game/Masks/T_ForestDensity",
                                                bounds, channel_index=0,
                                                threshold=0.2, seed=42)

# 3) Two-tier split: hero ≥ 15m apart, filler in between
hero    = P.filter_points_by_min_distance(modulated, min_dist=1500.0)
hero_set = set((round(p.x, 1), round(p.y, 1)) for p in hero)
filler  = [p for p in modulated if (round(p.x, 1), round(p.y, 1)) not in hero_set]

# 4) Jittered transforms (deterministic given seed)
def to_xforms(pts, smin, smax, seed):
    base = [u.Transform(p, u.Rotator(0, 0, 0), u.Vector(1, 1, 1)) for p in pts]
    return P.jitter_transforms(base, pos_sigma=20.0, rot_sigma=180.0,
                                scale_min=smin, scale_max=smax, seed=seed)
hero_xs = to_xforms(hero,   u.Vector(1.2, 1.2, 1.2), u.Vector(1.6, 1.6, 1.6), seed=1)
fill_xs = to_xforms(filler, u.Vector(0.7, 0.7, 0.7), u.Vector(1.1, 1.1, 1.1), seed=2)

# 5) Two HISMs + single nav rebuild per stub
hero_actor = P.ensure_procedural_ism_actor("Trees_Hero",   "/Game/Trees/SM_Pine_Large", use_hism=True)
fill_actor = P.ensure_procedural_ism_actor("Trees_Filler", "/Game/Trees/SM_Pine_Small", use_hism=True)
P.add_instances_by_transforms(hero_actor, hero_xs, world_space=True)
P.add_instances_by_transforms(fill_actor, fill_xs, world_space=True)
P.rebuild_procedural_navigation(hero_actor)
P.rebuild_procedural_navigation(fill_actor)
```

**Spline-driven placement** (lampposts every 20m along a road, mesh forward = +X):
```python
xs = P.sample_transforms_along_spline("BP_RoadSpline", "", "by_distance", 2000.0)
actor = P.ensure_procedural_ism_actor("Lampposts", "/Game/Props/SM_Lamppost", use_hism=False)
P.add_instances_by_transforms(actor, xs, world_space=True)
```

**Iterative refinement with stable IDs**:
```python
ids = P.add_instances_by_transforms(actor, initial_xs, world_space=True)
# ... user reviews; some look wrong
P.remove_instances_by_ids(actor, [ids[i] for i in problematic])
# Or shift one
P.update_instance_transforms_by_ids(actor, [ids[7]], [shifted_xform], world_space=True)
```

**Volumetric scatter** (cubes inside a TriggerVolume):
```python
pts3d = P.sample_points_in_volume("BP_PlayableArea", count=300, seed=42, max_attempts=30)
# Combine with Poisson 3D for spaced asteroids:
field = u.Box(u.Vector(0,0,0), u.Vector(5000, 5000, 2000))
asteroids = P.sample_points_poisson_disk3d(field, min_radius=200.0, max_attempts=30, seed=42)
```

**Forthcoming** (per `docs/plans/procedural-content-roadmap.md`):
- Lane 2 — Geometry Script wrap (`UnrealBridgeGeometryLibrary`), 5.7-only — mesh authoring (boolean / displace / smooth / decimate / UV unwrap), the "build geometry from script" half of the system.
