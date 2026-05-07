# bridge-procedural-api

`unreal.UnrealBridgeProceduralLibrary` — procedural placement primitives (sampling / instancing) for level dressing. Call from Python to compose scatter pipelines without touching PCG graphs. See `docs/plans/procedural-content-roadmap.md` for the full design contract.

Hard contract:
- All sampling functions are **deterministic** given `(params, seed)` — internal `FRandomStream(seed)`, never `FMath::Rand`.
- Operates on the **editor world** (level dressing must survive PIE exit). For PIE-time scatter, use generic line traces from `UnrealBridgeLevelLibrary` directly.
- Trace-based sampling uses `ECC_Visibility` + `bTraceComplex=true`, matching the existing trace family.
- Hard cap of **100,000 points** per call. Above that, return empty + log warning. The 100k+ scale is PCG's territory.

**P0 fully shipped (11/11)**: M1-1 grid, M1-2 Poisson2D, M1-4 surface, M1-5 landscape, M2-1 slope, M2-3 min-distance, M2-7 project-to-surface, M3-1 ensure ISM, M3-2 add instances, M3-5 clear, M3-6 rebuild nav. P1 (rest of M1/M2/M3) and Lane 2 (Geometry Script wrap) per `docs/plans/procedural-content-roadmap.md`.

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

## End-to-end recipe (P0 complete)

The full Lane 1 P0 chain — naturalistic forest scatter on a Landscape, slope-filtered, nav-rebuilt:

```python
import unreal as u
P = u.UnrealBridgeProceduralLibrary

# 200m × 200m landscape region
bounds = u.Box.build_aabb(u.Vector(0, 0, 0), u.Vector(20000, 20000, 0))

# 1) Sample with natural spacing (Poisson) — guarantees ≥ 5m between trees
poisson = P.sample_points_poisson_disk2d(bounds, min_radius=500.0, max_attempts=30, seed=42)

# 2) Drape onto landscape surface
projected, normals = P.project_points_to_surface(poisson, 5000.0, 5000.0)

# 3) Drop anything on a > 30° slope
walkable = P.filter_points_by_slope(projected, max_slope_deg=30.0, bounce_up=5000.0)
print(f"Poisson {len(poisson)} → projected {len(projected)} → ≤30° {len(walkable)}")

# 4) Build transforms (uniform rot/scale here; jitter_transforms M1-10 forthcoming)
xs = [u.Transform(p, u.Rotator(0,0,0), u.Vector(1,1,1)) for p in walkable]

# 5) Spawn HISM stub + add all instances in one batch (nav update deferred)
actor = P.ensure_procedural_ism_actor("Forest_Pines", "/Game/Trees/SM_Pine", use_hism=True)
ids = P.add_instances_by_transforms(actor, xs, world_space=True)

# 6) Single end-of-batch nav rebuild (cheap vs N per-call updates)
P.rebuild_procedural_navigation(actor)
print(f"Placed {len(ids)} pines, nav rebuilt")
```

**Two-pass thinning** (post-Poisson second filter at a wider scale):
```python
# Already have ≥ 5m via Poisson; further thin to ≥ 15m for hero trees
hero = P.filter_points_by_min_distance(projected, min_dist=1500.0)
filler = [p for p in projected if p not in set(map(tuple, hero))]  # rough complement
# place hero with SM_Pine_Large, filler with SM_Pine_Small — two HISMs
```

**Landscape direct path** (skip Poisson + project, single faster call):
```python
# 5-10x faster when target is a Landscape — height comes from heightmap, not line trace
pts = P.sample_points_on_landscape("Landscape_0", bounds, count=2000, seed=42)
# pts already has correct surface Z; jump straight to filter / instance
```

**Forthcoming** (per `docs/plans/procedural-content-roadmap.md` P1+):
- `jitter_transforms` (M1-10) — randomize rotation/scale per instance
- `filter_points_by_density_mask` (M2-4) — texture-based density modulation
- `filter_points_by_overlap` (M2-2) — keep/reject points near specific actor classes
- `sample_points_in_volume` (M1-7) — volumetric reject sampling
- Lane 2 — Geometry Script wrap (`UnrealBridgeGeometryLibrary`), 5.7-only
