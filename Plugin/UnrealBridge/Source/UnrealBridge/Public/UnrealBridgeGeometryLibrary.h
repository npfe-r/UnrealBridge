#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealBridgeGeometryLibrary.generated.h"

class UMaterialInterface;

/**
 * Static facts about a UDynamicMesh — what M4-6 returns. Field names follow
 * the standard UE Python snake_case projection, so e.g. `bHasNormals` becomes
 * `.has_normals` (NOT `.b_has_normals` — see feedback_ue_python_bool_prefix).
 */
USTRUCT(BlueprintType)
struct FBridgeMeshInfo
{
	GENERATED_BODY()

	/** Triangle count from `UDynamicMesh::GetTriangleCount()` (NOT NumTriangleIDs — gaps after deletes excluded). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry")
	int32 NumTriangles = 0;

	/** Vertex count from `MeshQueryFunctions::GetVertexCount` (NOT NumVertexIDs — gaps after deletes excluded). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry")
	int32 NumVertices = 0;

	/** Number of UV channels (`MeshQueryFunctions::GetNumUVSets`). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry")
	int32 NumUVLayers = 0;

	/** True when the mesh has the per-triangle Normals attribute enabled (`GetHasTriangleNormals`). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry")
	bool bHasNormals = false;

	/** True when the Vertex Colors attribute is enabled (`GetHasVertexColors`). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry")
	bool bHasVertexColors = false;

	/** Local-space bounding box (`MeshQueryFunctions::GetMeshBoundingBox`). Empty when handle is invalid. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry")
	FBox Bounds = FBox(ForceInit);
};

/**
 * Geometry Script wrapper — Lane 2 of the procedural-content roadmap.
 *
 * Design contract (see docs/plans/procedural-content-roadmap.md §4):
 *  - UDynamicMesh handles are int32 keys into a process-global map. The
 *    map holds TStrongObjectPtr<UDynamicMesh> so handles survive GC for
 *    as long as the bridge keeps them registered. Caller must Release()
 *    when done.
 *  - All ops run on GameThread (bridge worker dispatches there); large
 *    sweeps (M5-6 voxel merge, M5-2 boolean on dense meshes) can block
 *    the GT for seconds. Split them across exec calls per
 *    feedback_bridge_exec_holds_gamethread.
 *  - The whole library is gated to UE 5.7+. On 5.4-5.6 the UFUNCTIONs
 *    still exist (UHT requires unconditional declarations) but their
 *    bodies live in UnrealBridgeGeometryLibrary_Stubs.cpp and log a
 *    warning + return T{}. See feedback_uht_no_if_around_reflection.
 *  - Asset save UFUNCTIONs (M4-4 / M4-5) must run in editor world only;
 *    invoking them from PIE / cmdlet silently no-ops.
 *
 * UFUNCTIONs land in three batches:
 *   Phase 2 — M4: handle pool + asset I/O (8 ops) — THIS BATCH
 *   Phase 3 — M5/M6 P2 priority: boolean / smooth / decimate / recompute_normals (4 ops)
 *   Future  — M5 rest + M6-1/2 bake (12 ops)
 */
UCLASS()
class UNREALBRIDGE_API UUnrealBridgeGeometryLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	// ─── M4 — Handle pool + asset I/O ────────────────────────────

	/**
	 * Allocate a fresh, empty UDynamicMesh and register it under a new int32 handle.
	 * Caller must release the handle via ReleaseDynamicMesh when done.
	 *
	 * @return Positive handle (1+); 0 indicates allocation failure (very unlikely).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static int32 CreateDynamicMesh();

	/**
	 * Drop a handle from the registry. The underlying UDynamicMesh becomes eligible
	 * for GC once no other reference holds it. Idempotent — releasing an unknown
	 * handle just returns false.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool ReleaseDynamicMesh(int32 Handle);

	/**
	 * List all currently-registered handles. Useful for debugging leaks ("did I
	 * forget to release?") or batch cleanup before shutdown.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static TArray<int32> ListDynamicMeshHandles();

	/**
	 * Replace the contents of the handle's mesh with the geometry of a StaticMesh asset.
	 * Wraps `UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2` with
	 * default options (apply build settings, request tangents, use section materials).
	 *
	 * @param Handle    Bridge handle (must be live).
	 * @param AssetPath Content path of the StaticMesh asset, e.g. "/Engine/BasicShapes/Cube".
	 * @param Lod       LOD index against the source-model LOD chain. Silently clamped
	 *                  to the available LOD count.
	 * @return true on Success outcome from Geometry Script; false on Failure (asset not
	 *         loadable, handle invalid, source-model LODs unavailable at runtime).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool LoadMeshFromStaticMesh(int32 Handle, const FString& AssetPath, int32 Lod = 0);

	/**
	 * Pull mesh geometry from a primitive component on a level actor. Prefer this over
	 * LoadMeshFromStaticMesh when the actor's component has runtime overrides
	 * (instance-specific materials, deformations, etc.). Wraps
	 * `USceneUtilityFunctions::CopyMeshFromComponent` with default options
	 * (want normals + tangents + UVs + colors).
	 *
	 * @param ActorLabel    User-visible label or FName of the level actor (matches
	 *                      `UnrealBridgeLevelLibrary` resolver semantics).
	 * @param ComponentName FName of the component on that actor. Empty string falls
	 *                      back to the root component.
	 * @param Handle        Target bridge handle (must be live).
	 * @return true on Success outcome; false if actor / component / handle is unknown.
	 *
	 * Editor world only; invoking from PIE returns false + logs a warning.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool LoadMeshFromComponent(const FString& ActorLabel, const FString& ComponentName, int32 Handle);

	/**
	 * Save the handle's mesh as a brand-new StaticMesh asset on disk. Wraps
	 * `UGeometryScriptLibrary_CreateNewAssetUtilityFunctions::CreateNewStaticMeshAssetFromMesh`.
	 * Editor-only — `WITH_EDITOR` gate in the body returns "" outside the editor.
	 *
	 * Per project_split_asset_ops feedback: do NOT chain this with mesh ops in a
	 * single bridge exec. CreateNewStaticMeshAssetFromMesh triggers an asset-reference
	 * completing modal that deadlocks the GameThread when other ops are waiting in
	 * the queue. Pattern:
	 *   exec1: load + transform/edit; exec2: save_to_new
	 *
	 * @param Handle       Source bridge handle.
	 * @param NewAssetPath Content path for the new asset, e.g. "/Game/_BridgeTest/SM_Out".
	 * @param MaterialList Optional materials for the new asset's slots. Order maps
	 *                     1:1 to the dynamic mesh's MaterialIDs (slot 0 ← idx 0, etc.).
	 *                     Empty list → engine assigns default material to every slot.
	 * @return The saved asset's package path on Success; "" on Failure.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static FString SaveMeshToNewStaticMesh(int32 Handle, const FString& NewAssetPath, const TArray<UMaterialInterface*>& MaterialList);

	/**
	 * Push the handle's mesh INTO an existing StaticMesh asset, replacing its
	 * source-model LOD0 geometry. Wraps `UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh`.
	 * Editor-only.
	 *
	 * Calls `Modify()` + `MarkPackageDirty()` on the target so undo + Save All pick
	 * up the change. Caller is still responsible for `editor_asset_lib.save_loaded_asset(...)`
	 * if persistence to disk is required.
	 *
	 * @param Handle             Source bridge handle.
	 * @param ExistingAssetPath  Content path to an EXISTING StaticMesh asset.
	 * @param bReplaceMaterials  When true, the dynamic mesh's MaterialIDs replace the
	 *                           existing material slot list. When false, slot mapping
	 *                           is preserved by section index (most common).
	 * @return true on Success outcome; false otherwise.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool SaveMeshToExistingStaticMesh(int32 Handle, const FString& ExistingAssetPath, bool bReplaceMaterials);

	/** Static facts about the mesh held by Handle. Returns an all-zero struct when handle invalid. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static FBridgeMeshInfo GetMeshInfo(int32 Handle);

	// ─── M5/M6 — P2 priority subset (boolean / smooth / decimate / recompute_normals) ─

	/**
	 * Boolean two meshes in-place on `HandleA`. `HandleB` is the tool mesh and is
	 * untouched. Wraps `MeshBooleanFunctions::ApplyMeshBoolean` with default options
	 * (fill holes, simplify output, no weld). Both transforms are identity — apply
	 * `mesh_transform` (Phase 3+) ahead of time if the meshes need world-space
	 * placement to overlap correctly.
	 *
	 * @param HandleA Target mesh (modified in place).
	 * @param HandleB Tool mesh (read-only).
	 * @param Op One of "union" / "intersect" / "intersection" / "subtract"
	 *           (case-insensitive). Unknown values fail loudly + return false.
	 * @return true on Success outcome.
	 *
	 * Cost: O(NA × NB) worst case for the BSP tree intersection. Dense meshes
	 * (>50k tri × 50k tri) can block the GameThread for several seconds — split
	 * into separate exec calls for chained ops (per feedback_bridge_exec_holds_gamethread).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool MeshBoolean(int32 HandleA, int32 HandleB, const FString& Op);

	/**
	 * Iterative Laplacian smoothing in-place. Wraps `MeshDeformFunctions::ApplyIterativeSmoothingToMesh`
	 * with empty selection (whole mesh) and `Alpha` interpreted as `Strength`
	 * (clamped to [0, 1]).
	 *
	 * @param Handle Target mesh.
	 * @param Iterations Smoothing passes (≥1). Higher = smoother but more shrinkage.
	 *                   Engine default is 10; common practical range 2-20.
	 * @param Strength Per-iteration weight in [0, 1]. 0 = no movement; 1 = full
	 *                 Laplacian step. Engine default is 0.2.
	 * @return true on Success outcome.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool MeshSmooth(int32 Handle, int32 Iterations, float Strength);

	/**
	 * Reduce mesh down to a target triangle count. Wraps
	 * `MeshSimplifyFunctions::ApplySimplifyToTriangleCount` with default
	 * AttributeAware options.
	 *
	 * @param Handle Target mesh.
	 * @param TargetTris Desired final triangle count (≥4). The simplifier may
	 *                   not hit it exactly — typically lands within ±5%.
	 * @return true on Success outcome.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool MeshDecimate(int32 Handle, int32 TargetTris);

	/**
	 * Recompute normals and tangents in-place. When `AngleThresholdDeg > 0`,
	 * applies hard-edge splitting first (`ComputeSplitNormals` with the given
	 * opening angle) — this is what you almost always want after a boolean,
	 * decimate, or sweep, since those operations leave smooth-shaded seams
	 * everywhere. With `AngleThresholdDeg ≤ 0`, falls back to plain
	 * `RecomputeNormals` (no hard edges).
	 *
	 * Tangents are always recomputed via `ComputeTangents` after the normal step.
	 *
	 * @param Handle Target mesh.
	 * @param AngleThresholdDeg Opening-angle threshold (degrees) for hard-edge
	 *                          detection. Common values: 30-60 for organic, 45
	 *                          for hard-surface. Pass 0 or negative to skip
	 *                          split-normals.
	 * @return true on Success outcome.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Geometry")
	static bool RecomputeNormalsAndTangents(int32 Handle, float AngleThresholdDeg);
};
