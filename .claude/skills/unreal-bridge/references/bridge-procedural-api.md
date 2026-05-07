# bridge-procedural-api

`unreal.UnrealBridgeProceduralLibrary` — procedural placement primitives (sampling / instancing) for level dressing. Call from Python to compose scatter pipelines without touching PCG graphs. See `docs/plans/procedural-content-roadmap.md` for the full design contract.

Hard contract:
- All sampling functions are **deterministic** given `(params, seed)` — internal `FRandomStream(seed)`, never `FMath::Rand`.
- Operates on the **editor world** (level dressing must survive PIE exit). For PIE-time scatter, use generic line traces from `UnrealBridgeLevelLibrary` directly.
- Trace-based sampling uses `ECC_Visibility` + `bTraceComplex=true`, matching the existing trace family.
- Hard cap of **100,000 points** per call. Above that, return empty + log warning. The 100k+ scale is PCG's territory.

Currently shipped (M1-1, M1-4, M3-1, M3-2, M3-5 — first vertical slice). Forthcoming per roadmap: M1-2 Poisson2D, M1-5 Landscape direct sampling, M2-1/3/7 filter family, M3-6 nav rebuild.

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

## ensure_procedural_ism_actor(tag, mesh_path, b_use_hism) -> str

Find or create a stub `AActor` carrying a single `UInstancedStaticMeshComponent` (or `UHierarchicalInstancedStaticMeshComponent` if `b_use_hism`) configured to render `mesh_path`. Idempotent — calling twice with the same `tag` returns the same actor.

| Param | Type | Notes |
|---|---|---|
| `tag` | str | Unique grouping key. The created actor carries this as `FName` in its `Tags` list. |
| `mesh_path` | str | `/Game/...` content path of the StaticMesh asset. |
| `b_use_hism` | bool | `True` for HISM (with culling/LOD; preferred for foliage-density scatter). `False` for plain ISM (no hierarchy). |

Returns: actor label (e.g. `"Procedural_Trees_PineForest"`), or empty string on failure.

**Cost** — O(1) on reuse, single `SpawnActor` + `NewObject` + `RegisterComponent` on first call. ~1-2ms.

**Example**
```python
actor = P.ensure_procedural_ism_actor("Trees_PineForest",
    "/Game/Trees/SM_Pine", b_use_hism=True)
# Subsequent call returns the same actor — idempotent.
same = P.ensure_procedural_ism_actor("Trees_PineForest", "/Game/Trees/SM_Pine", True)
assert actor == same
```

**Pitfalls**
- **Tag-based lookup is mesh-blind**: if an actor already exists with `tag`, this function returns it *regardless* of whether its current mesh matches `mesh_path`. To swap the mesh on an existing stub, destroy the actor first via `unreal.UnrealBridgeLevelLibrary.destroy_actor(label)` then re-call this function.
- Spawned class is `AActor`, not `AStaticMeshActor` — `AStaticMeshActor`'s `RootComponent` is a plain `UStaticMeshComponent` and cannot be swapped to ISM. The stub looks empty in the World Outliner's class column ("Actor") — that's intentional.
- `FScopedTransaction` is wrapped, so Ctrl+Z undoes the actor creation.

---

## add_instances_by_transforms(actor_name, xs, b_world_space) -> list[int32]

Bulk-add instances to a procedural ISM actor's `[H]ISMC` in one call. Returns the instance index of each transform in the order added — required for later `update`/`remove` ops.

| Param | Type | Notes |
|---|---|---|
| `actor_name` | str | Label returned by `ensure_procedural_ism_actor`. |
| `xs` | `list[Transform]` | Instance transforms. |
| `b_world_space` | bool | `True` = `xs` are world-space; component handles the inverse-transform internally. `False` = local. Pick whichever your sampling pipeline produced. |

Returns: instance ID list, parallel to `xs`. Empty on actor-not-found / no-ISM / empty input.

**Cost** — One `AddInstances` call, internally batched. ~1ms per 1000 instances on HISM.

**Example**
```python
xs = [u.Transform(p, u.Rotator(0,0,0), u.Vector(1,1,1)) for p in pts]
ids = P.add_instances_by_transforms("Procedural_Trees_PineForest", xs, b_world_space=True)
print(f"added {len(ids)} instances, first id={ids[0]}")
```

**Pitfalls**
- `b_should_return_indices=true` is forced internally — without it the Engine API silently returns no IDs (perf optimisation), and you lose the ability to update/remove individual instances later.
- `b_update_navigation=false` is forced — call the forthcoming `rebuild_procedural_navigation` (M3-6) once at end-of-batch instead of N times mid-batch. This cuts nav rebuild cost from O(N) to O(1).
- Instance IDs are stable across `add` / `remove` cycles only if you remove with `remove_instances_by_ids` (M3-3, forthcoming). Direct `ClearInstances` resets the ID space.

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

## End-to-end recipe (current slice)

The 5 functions shipped here cover "spawn N grid-aligned things on a surface and clear them later":

```python
import unreal as u
P = u.UnrealBridgeProceduralLibrary

# Pick a 100m x 100m region, sample at 5m spacing
bounds = u.Box.build_aabb(u.Vector(0, 0, 0), u.Vector(5000, 5000, 0))
grid = P.sample_points_grid(bounds, spacing=500.0, jitter_ratio=0.3, seed=42)

# Drape onto landscape (or any surface actor)
pts = P.sample_points_on_surface("LandscapeActor", count=len(grid), seed=42, max_bounce_up=5000.0)

# Convert to transforms (no rotation/scale variation yet — that's M1-10 jitter_transforms)
xs = [u.Transform(p, u.Rotator(0,0,0), u.Vector(1,1,1)) for p in pts]

# Spawn HISM stub + add instances
actor = P.ensure_procedural_ism_actor("Demo_Rocks", "/Game/Env/SM_Rock_01", b_use_hism=True)
ids = P.add_instances_by_transforms(actor, xs, b_world_space=True)
print(f"placed {len(ids)} rocks")

# Iterate by clearing + re-running with a different seed
P.clear_instances(actor)
```

**Forthcoming** (per `docs/plans/procedural-content-roadmap.md` P0):
- `sample_points_poisson_disk_2d` — Bridson algorithm for natural distribution
- `sample_points_on_landscape` — 5-10x faster Landscape-direct sampling
- `filter_points_by_slope` / `filter_points_by_min_distance` / `project_points_to_surface` — filter chain
- `rebuild_procedural_navigation` — single nav rebuild at end of placement batch
