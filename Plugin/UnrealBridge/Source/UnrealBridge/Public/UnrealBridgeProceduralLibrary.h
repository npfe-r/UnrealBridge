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
};
