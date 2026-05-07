#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealBridgeProceduralLibrary.generated.h"

/**
 * Procedural content authoring primitives — sampling / filter / instancing —
 * intentionally NOT a PCG-graph wrapper.
 *
 * Design contract (see docs/plans/procedural-content-roadmap.md):
 *  - Point-list in / point-list out (callable from Python as plain arrays).
 *  - All sampling functions are deterministic given (params, seed); they
 *    construct an FRandomStream(Seed) at entry and never touch FMath::Rand.
 *  - Operates on the **editor world** by default (level dressing must
 *    survive PIE exit). Trace-based sampling against world geometry uses
 *    `ECC_Visibility` + `bTraceComplex=true`, matching the existing
 *    UnrealBridgeLevelLibrary trace family for cross-call consistency.
 *  - Hard cap on points: 100k per call. Above that → return empty + log.
 *    The 100k+ scale belongs to PCG, not to this bridge layer.
 */
UCLASS()
class UNREALBRIDGE_API UUnrealBridgeProceduralLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// ─── M1 — Sampling ───────────────────────────────────────

	/**
	 * Regular grid + per-cell jitter. Deterministic given (Bounds, Spacing, JitterRatio, Seed).
	 *
	 * @param Bounds       Axis-aligned box. Z component is preserved as the output Z.
	 * @param Spacing      Cell size (cm). Must be > 0; <=0 returns empty.
	 * @param JitterRatio  Per-cell offset as fraction of Spacing, clamped to [0, 0.5].
	 *                     0 = strict lattice; 0.5 = each point can land anywhere in its cell.
	 * @param Seed         FRandomStream seed. Same params + same seed = same output.
	 * @return XY-jittered grid of points at (Bounds.Min.Z + Bounds.Max.Z)/2. Empty on bad input
	 *         or if would-be point count exceeds 100,000 (logs a warning).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsGrid(FBox Bounds, float Spacing, float JitterRatio, int32 Seed);

	/**
	 * Random points draped on the surface of a target actor via downward line trace.
	 *
	 * Workflow: take actor world bounds → draw `Count` uniform-random XY points within
	 * the bounds → from each (X, Y, Z+MaxBounceUp) trace down `ECC_Visibility` with
	 * `bTraceComplex=true` to (X, Y, Bounds.Min.Z - MaxBounceUp) → push hit points,
	 * drop misses. Output count <= Count (hits only).
	 *
	 * Use this for general meshes. For Landscape specifically prefer SamplePointsOnLandscape
	 * (forthcoming, M1-5) which is 5-10x faster via direct height-query API.
	 *
	 * @param ActorLabel   Target actor's user-visible label (or FName).
	 * @param Count        Number of trace attempts. Capped at 100,000.
	 * @param Seed         FRandomStream seed.
	 * @param MaxBounceUp  cm to add above bounds top before tracing down. 5000 covers typical
	 *                     terrain undulation; raise for very tall actors.
	 * @return Hit point list. Empty if actor not found / 0 hits / Count > cap.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsOnSurface(const FString& ActorLabel, int32 Count, int32 Seed, float MaxBounceUp);

	/**
	 * Random points on a Landscape via direct height-query API. 5-10x faster than
	 * `SamplePointsOnSurface` for landscape targets because it bypasses line-trace
	 * and goes straight to `ALandscapeProxy::GetHeightAtLocation`. Hit rate is
	 * effectively 100% inside Landscape coverage (vs <100% for trace-based which
	 * misses on holes/sky).
	 *
	 * Iterates **all** `LandscapeStreamingProxy` actors of the LandscapeInfo, so
	 * World Partition / Landscape Streaming sublevels are handled transparently —
	 * a sample point that falls outside the root proxy but inside a streaming
	 * proxy still gets a hit (plan §6 #5).
	 *
	 * Editor-only: depends on `ULandscapeInfo` lookup which is not populated in
	 * shipping builds. For PIE-time landscape sampling use `SamplePointsOnSurface`
	 * (M1-4) — slower but PIE-compatible.
	 *
	 * @param LandscapeLabel  Target Landscape actor's label or FName.
	 * @param Bounds2D        XY range to sample in. Z is ignored on input; output Z
	 *                        is the queried height per point.
	 * @param Count           Number of XY samples. Capped at 100,000.
	 * @param Seed            FRandomStream seed.
	 * @return Hit point list. Empty if landscape not found / Count > cap.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsOnLandscape(const FString& LandscapeLabel, FBox Bounds2D, int32 Count, int32 Seed);

	/**
	 * Bridson Poisson-disk sampling in 2D — every output point is at least
	 * `MinRadius` away from every other (in XY plane). Output count is **not**
	 * controllable directly: it depends on Bounds area, MinRadius, and the random
	 * rejection process. For natural-looking forest / scatter where regular grid
	 * spacing is visible.
	 *
	 * Algorithm: Bridson 2007. Background grid with cell = MinRadius/√2; per
	 * accepted point, generate `MaxAttempts` candidates in annulus [R, 2R],
	 * accept the first that has no neighbor within R via grid lookup. Worst-case
	 * O(N · MaxAttempts) where N = output count.
	 *
	 * @param Bounds       Box defining the XY sampling region. Z ignored on input;
	 *                     output Z = Bounds.Min.Z (caller pipes through
	 *                     ProjectPointsToSurface (M2-7) for ground contact).
	 * @param MinRadius    Minimum distance between any two points (cm). > 0 required.
	 * @param MaxAttempts  Per-active-point candidate retries. 30 is the canonical
	 *                     value from the Bridson paper; raising helps on tight
	 *                     packings.
	 * @param Seed         FRandomStream seed.
	 * @return Point set. Capped at 100,000.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsPoissonDisk2D(FBox Bounds, float MinRadius, int32 MaxAttempts, int32 Seed);

	/**
	 * 3D Bridson Poisson-disk: every output point is at least `MinRadius` from
	 * every other in 3D. For volumetric scatter (asteroid fields, particle clouds,
	 * floating debris). Same algorithm as M1-2 but with `cell = R/√3`, 5×5×5
	 * neighborhood scan, and spherical annulus candidate generation.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsPoissonDisk3D(FBox Bounds, float MinRadius, int32 MaxAttempts, int32 Seed);

	/**
	 * Sample evenly-spaced positions along a USplineComponent. `Mode` is either
	 * "by_count" (CountOrSpacing = N points evenly distributed end-to-end) or
	 * "by_distance" (CountOrSpacing = cm between consecutive points, including
	 * both endpoints). Output is in **world space**.
	 *
	 * @param SplineActorLabel  Actor owning the spline component.
	 * @param ComponentName     Name of the USplineComponent on that actor. Empty
	 *                          string = first SplineComponent found.
	 * @param Mode              "by_count" or "by_distance".
	 * @param CountOrSpacing    int(N) for by_count; cm spacing for by_distance.
	 *                          Bad values return empty.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsOnSpline(const FString& SplineActorLabel, const FString& ComponentName, const FString& Mode, float CountOrSpacing);

	/**
	 * Reject-sampled points inside a volume actor (typically `AVolume` subclass:
	 * TriggerVolume, BlockingVolume, BoxVolume etc). Falls back to actor bounds
	 * containment for non-volume actors (loose).
	 *
	 * @param VolumeActorLabel  Actor to sample inside. Must be loaded.
	 * @param Count             Target output count (≤ 100k cap).
	 * @param Seed              FRandomStream seed.
	 * @param MaxAttempts       Maximum reject-sample attempts before giving up
	 *                          (caps total work at Count × MaxAttempts).
	 *                          Default 30 if ≤ 0.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsInVolume(const FString& VolumeActorLabel, int32 Count, int32 Seed, int32 MaxAttempts);

	/**
	 * Per-cell jittered grid — the cheap O(N) alternative to Poisson when fully
	 * natural distribution isn't required. Splits XY bounds into
	 * `GridResolution × GridResolution` cells and picks one random point in each.
	 *
	 * @param Bounds           XY bounds; output Z = (Min.Z + Max.Z)/2.
	 * @param GridResolution   Cells per axis. Total points = GridResolution².
	 * @param Seed             FRandomStream seed.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> SamplePointsJitterStratified(FBox Bounds, int32 GridResolution, int32 Seed);

	/**
	 * Same as SamplePointsOnSpline but yields full FTransform: location +
	 * rotation aligned to spline tangent. The standard "lampposts / fence
	 * posts / road markers" placement primitive.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FTransform> SampleTransformsAlongSpline(const FString& SplineActorLabel, const FString& ComponentName, const FString& Mode, float CountOrSpacing);

	/**
	 * Post-process jitter on an existing transform list. Adds Gaussian noise to
	 * position (PosSigma cm, Box-Muller standard normal scaled), Gaussian noise
	 * to euler rotation (RotSigma degrees, applied per axis), and uniform random
	 * scale per-axis between `ScaleMin` and `ScaleMax` (component-wise).
	 *
	 * Deterministic given (Xs, params, Seed).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FTransform> JitterTransforms(const TArray<FTransform>& Xs, float PosSigma, float RotSigma, FVector ScaleMin, FVector ScaleMax, int32 Seed);

	// ─── M2 — Filter ─────────────────────────────────────────

	/**
	 * Drop points that sit on terrain steeper than `MaxSlopeDeg`. For each input
	 * point, traces down through the ground beneath it, takes the impact normal,
	 * and keeps the point only when the angle between that normal and +Z is at
	 * most `MaxSlopeDeg`.
	 *
	 * Misses (no surface within `BounceUp`/`BounceUp` range) are dropped — without
	 * a surface there's no slope to evaluate.
	 *
	 * @param In            Input points.
	 * @param MaxSlopeDeg   Maximum slope angle (degrees, 0..90). 0 = perfectly flat
	 *                      only; 90 = no filtering.
	 * @param BounceUp      cm above each point to start the trace; trace endpoint
	 *                      is the symmetric distance below. 5000 typical.
	 * @return Filtered list — original points (NOT projected). Use
	 *         `ProjectPointsToSurface` (M2-7) afterwards if you want ground-snapped
	 *         output too.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> FilterPointsBySlope(const TArray<FVector>& In, float MaxSlopeDeg, float BounceUp);

	/**
	 * Greedy first-come-wins thinning: scan input in order, keep a point only if
	 * no already-kept point is within `MinDist` of it (XY-distance only — matches
	 * the 2D model used by `SamplePointsPoissonDisk2D`). Useful as a post-Poisson
	 * second pass with a different scale, or to thin grids before instancing.
	 *
	 * Implementation uses a flat XY grid bucket (`cell = MinDist/√2`, 5×5
	 * neighborhood scan) — O(N) amortized for typical density. For inputs whose
	 * XY bounds × density would require > 10× the 100k-point cap of cells, refuses
	 * and returns empty (raise `MinDist` or shrink input).
	 *
	 * @param In       Input points.
	 * @param MinDist  Minimum XY distance to maintain between kept points (cm).
	 *                 ≤ 0 = pass-through (no filtering).
	 * @return Filtered list — order-preserving among kept points.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> FilterPointsByMinDistance(const TArray<FVector>& In, float MinDist);

	/**
	 * Project each input point vertically onto the surface beneath it via
	 * line-trace. The standard "after Poisson2D, drape onto terrain" finishing
	 * step; also outputs the surface normal at each hit for downstream alignment.
	 *
	 * For each `In[i]`:
	 *   from (x, y, z + BounceUp) trace down to (x, y, z - BounceDown);
	 *   on hit  → push impact point + impact normal;
	 *   on miss → push original point + +Z normal (degenerate fallback).
	 *
	 * Output array is **always parallel** to In — same length, no filtering. Use
	 * this when you want to keep all points and just snap them; for true filtering
	 * pair with `FilterPointsBySlope` / `FilterPointsByOverlap` (forthcoming M2-1
	 * / M2-2).
	 *
	 * @param In             Input point list.
	 * @param BounceUp       cm above each input Z to start the trace. 5000 typical.
	 * @param BounceDown     cm below each input Z as the trace endpoint. 5000 typical.
	 * @param OutHitNormals  [out] Parallel array of impact normals (or +Z on miss).
	 * @return Projected points. Same length as In.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> ProjectPointsToSurface(const TArray<FVector>& In, float BounceUp, float BounceDown, TArray<FVector>& OutHitNormals);

	/**
	 * Drop points whose sphere-overlap (radius `Radius` cm) hits any actor of any
	 * class in `BlockingClassPaths`. The "avoid roads / buildings / water" filter.
	 *
	 * @param Pts                 Input points.
	 * @param BlockingClassPaths  Class paths (e.g. "/Game/BP/BP_Building.BP_Building_C")
	 *                            or short class names. Resolved at start; bad
	 *                            entries are silently skipped.
	 * @param Radius              Sphere overlap test radius in cm. > 0 required.
	 * @return Filtered list — points NOT overlapping any blocking actor.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> FilterPointsByOverlap(const TArray<FVector>& Pts, const TArray<FString>& BlockingClassPaths, float Radius);

	/**
	 * Texture-driven density modulation. Maps each point's XY into the texture
	 * via `BoundsXY` → UV, samples the requested channel of mip 0, and keeps the
	 * point with stochastic probability based on the texel value.
	 *
	 * Selection rule:
	 *   1. Sample channel value c ∈ [0, 1].
	 *   2. If c < Threshold → drop (hard cutoff; default 0 means no cutoff).
	 *   3. Else keep with probability c (stochastic — high mask values pass more).
	 *
	 * The texture **must be CompressionSettings = TC_VectorDisplacementmap** so
	 * its raw RGBA8 bulk data is readable on the CPU; other compression formats
	 * (DXT/BC) return empty + log warning. Plan §6 #11.
	 *
	 * @param Pts            Input points.
	 * @param TextureAsset   `/Game/...` content path of UTexture2D.
	 * @param BoundsXY       World-space XY range that maps to the texture's [0,1]
	 *                       UV space. Z ignored.
	 * @param ChannelIndex   0=R, 1=G, 2=B, 3=A. Out-of-range falls back to R.
	 * @param Threshold      Hard cutoff [0, 1]. 0 = pure stochastic; 0.5 = require
	 *                       mask ≥ 0.5 before any pass.
	 * @param Seed           FRandomStream seed for the stochastic step.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> FilterPointsByDensityMask(const TArray<FVector>& Pts, const FString& TextureAsset, FBox BoundsXY, int32 ChannelIndex, float Threshold, int32 Seed);

	/**
	 * Keep or drop points based on whether they're inside `ContainerActorLabel`'s
	 * collision shape. For `AVolume` and subclasses uses `EncompassesPoint`
	 * (accurate); for any other actor falls back to actor bounds containment
	 * (loose AABB test).
	 *
	 * @param Pts                  Input points.
	 * @param ContainerActorLabel  Actor to test against.
	 * @param bInside              `true` = keep points inside the container;
	 *                             `false` = keep points outside.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> FilterPointsInsideActor(const TArray<FVector>& Pts, const FString& ContainerActorLabel, bool bInside);

	/**
	 * Drop points where the named landscape weightmap layer has weight below
	 * `Threshold`. Iterates landscape components and uses
	 * `EditorGetPaintLayerWeightByNameAtLocation` (editor-only) per point.
	 *
	 * @param Pts             Input points.
	 * @param LandscapeLabel  ALandscape / ALandscapeProxy actor label.
	 * @param LayerName       Paint layer name (FName), e.g. "Grass", "Forest".
	 * @param Threshold       Minimum weight [0, 1] to keep (1 = mask must be
	 *                        fully painted; 0 = always pass).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<FVector> FilterPointsByLandscapeLayer(const TArray<FVector>& Pts, const FString& LandscapeLabel, FName LayerName, float Threshold);

	// ─── M3 — Instancing ─────────────────────────────────────

	/**
	 * Find or create a stub Actor with the given Tag, owning a single
	 * `UInstancedStaticMeshComponent` (or `UHierarchicalInstancedStaticMeshComponent`
	 * if `bUseHISM`) configured to render `MeshPath`.
	 *
	 * Tag is the unique identifier — calling twice with the same Tag returns the same
	 * actor. The actor is spawned in the editor world so its instance set survives
	 * PIE exit and is serialised into the .umap.
	 *
	 * Implementation note: spawns `AActor` (NOT `AStaticMeshActor`, whose RootComponent
	 * is a plain SMC and cannot be swapped) and attaches the ISM/HISM as RootComponent
	 * via `RegisterComponent` + `AddInstanceComponent`. See
	 * `UnrealBridgeLevelLibrary.cpp::AddComponentOfClass` for the canonical pattern.
	 *
	 * @return Actor label, or empty FString on failure (no editor world / mesh load fail).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static FString EnsureProceduralISMActor(const FString& Tag, const FString& MeshPath, bool bUseHISM);

	/**
	 * Bulk-add instances to a procedural ISM actor's [H]ISMC. One round-trip = one
	 * `AddInstances(... bShouldReturnIndices=true ...)` call (NOT a Python loop of
	 * single-instance adds — that has O(N²) BVH rebuild cost on HISM).
	 *
	 * Navigation rebuild is **deferred** (`bUpdateNavigation=false`) — call
	 * `RebuildProceduralNavigation` (forthcoming M3-6) once at the end of a placement
	 * batch instead of N times.
	 *
	 * @param ActorName    Actor label (returned by EnsureProceduralISMActor).
	 * @param Xs           World-space (or local-space if bWorldSpace=false) transforms.
	 * @param bWorldSpace  When true, Xs are interpreted as world-space; the component
	 *                     converts internally. Match this with the space your sampling
	 *                     pipeline produced.
	 * @return Instance indices in the same order as Xs. Empty on failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static TArray<int32> AddInstancesByTransforms(const FString& ActorName, const TArray<FTransform>& Xs, bool bWorldSpace);

	/**
	 * Remove every instance from the actor's [H]ISMC and dirty the render state.
	 * Wrapped in `FScopedTransaction` so undo/redo restores the previous instance set.
	 * Does not destroy the actor itself — the empty stub remains, ready for the next
	 * AddInstancesByTransforms.
	 *
	 * @return true iff actor + ISM resolved and `ClearInstances` was called.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static bool ClearInstances(const FString& ActorName);

	/**
	 * Bulk-remove instances by ID. Sorts the IDs in reverse and dedupes before
	 * calling `RemoveInstances(SortedReverse, true)` — passing `true` for the
	 * `bAlreadySortedReverse` flag, the canonical UE pattern that avoids
	 * per-removal swap-back errors. Negative IDs are silently dropped.
	 *
	 * @return true iff actor + ISM resolved (even if all IDs were stale).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static bool RemoveInstancesByIds(const FString& ActorName, const TArray<int32>& InstanceIds);

	/**
	 * Bulk-update instance transforms by ID. Iterates `Ids` × `NewXs` (must be
	 * same length), calls `UpdateInstanceTransform` per instance with
	 * `bMarkRenderStateDirty=false`, then a single `MarkRenderStateDirty()` at
	 * the end — much cheaper than N per-call dirties.
	 *
	 * @return true iff lengths match + actor + ISM resolved.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static bool UpdateInstanceTransformsByIds(const FString& ActorName, const TArray<int32>& Ids, const TArray<FTransform>& NewXs, bool bWorldSpace);

	/**
	 * Single end-of-batch nav rebuild for a procedural ISM actor. Pairs with
	 * `AddInstancesByTransforms` whose `bUpdateNavigation=false` defers per-call
	 * nav updates — call this once after a placement batch instead of N times
	 * during it (plan §6 #2).
	 *
	 * Two effects:
	 * 1. For HISM, `BuildTreeIfOutdated(false, true)` synchronously rebuilds the
	 *    cluster tree (used by both rendering and nav-relevance queries). For
	 *    plain ISM this is a no-op (HISM-only API).
	 * 2. `FNavigationSystem::UpdateComponentData` re-registers the component with
	 *    the navigation octree, which is what `AddInstances(bUpdateNavigation=true)`
	 *    would have done internally per call.
	 *
	 * @return true iff the actor + its [H]ISMC resolved.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Procedural")
	static bool RebuildProceduralNavigation(const FString& ActorName);
};
