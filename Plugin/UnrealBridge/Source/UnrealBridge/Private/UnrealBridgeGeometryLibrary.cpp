#include "UnrealBridgeGeometryLibrary.h"

#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)

#include "UDynamicMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Editor.h"                          // GEditor
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"                     // TActorIterator
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "Misc/PackageName.h"

#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/SceneUtilityFunctions.h"
#include "GeometryScript/CreateNewAssetUtilityFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshVoxelFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "Engine/Texture2D.h"

#define LOCTEXT_NAMESPACE "UnrealBridgeGeometry"

namespace BridgeGeometryImpl
{
	// Process-global handle pool. UDynamicMesh* held via TStrongObjectPtr so
	// it survives GC until the caller releases the handle (pit #7 in roadmap).
	// Indexed by monotonically-increasing int — never reused so stale handles
	// fail loudly via the existence check rather than aliasing a fresh mesh.
	struct FHandlePool
	{
		TMap<int32, TStrongObjectPtr<UDynamicMesh>> Map;
		int32 NextHandle = 1;
	};

	FHandlePool& GetPool()
	{
		static FHandlePool Pool;
		return Pool;
	}

	UDynamicMesh* ResolveHandle(int32 Handle)
	{
		FHandlePool& Pool = GetPool();
		TStrongObjectPtr<UDynamicMesh>* Found = Pool.Map.Find(Handle);
		if (!Found)
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: handle %d is not registered"), Handle);
			return nullptr;
		}
		UDynamicMesh* Mesh = Found->Get();
		if (!Mesh)
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: handle %d resolved to null (already collected)"), Handle);
			return nullptr;
		}
		return Mesh;
	}

	UWorld* GetEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	// Mirror UnrealBridgeLevelLibrary actor-lookup conventions: try FName first,
	// then user-visible label. Editor world only.
	AActor* FindActor(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}
		const FName AsName(*NameOrLabel);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (!A)
			{
				continue;
			}
			if (A->GetFName() == AsName || A->GetActorLabel() == NameOrLabel)
			{
				return A;
			}
		}
		return nullptr;
	}

	bool ParseBooleanOp(const FString& Op, EGeometryScriptBooleanOperation& Out)
	{
		const FString Lower = Op.ToLower();
		if (Lower == TEXT("union"))                                  { Out = EGeometryScriptBooleanOperation::Union;        return true; }
		if (Lower == TEXT("intersect") || Lower == TEXT("intersection")) { Out = EGeometryScriptBooleanOperation::Intersection; return true; }
		if (Lower == TEXT("subtract") || Lower == TEXT("difference"))   { Out = EGeometryScriptBooleanOperation::Subtract;     return true; }
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge|Geometry: unknown boolean op '%s' (expected union / intersect / subtract)"),
			*Op);
		return false;
	}

	USceneComponent* FindComponent(AActor* Actor, const FString& ComponentName)
	{
		if (!Actor)
		{
			return nullptr;
		}
		if (ComponentName.IsEmpty())
		{
			return Actor->GetRootComponent();
		}
		const FName AsName(*ComponentName);
		TArray<USceneComponent*> Components;
		Actor->GetComponents<USceneComponent>(Components);
		for (USceneComponent* C : Components)
		{
			if (C && (C->GetFName() == AsName || C->GetName() == ComponentName))
			{
				return C;
			}
		}
		return nullptr;
	}
}

int32 UUnrealBridgeGeometryLibrary::CreateDynamicMesh()
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* NewMesh = NewObject<UDynamicMesh>(GetTransientPackage());
	if (!NewMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("UnrealBridge|Geometry: NewObject<UDynamicMesh> returned null"));
		return 0;
	}
	FHandlePool& Pool = GetPool();
	const int32 Handle = Pool.NextHandle++;
	Pool.Map.Add(Handle, TStrongObjectPtr<UDynamicMesh>(NewMesh));
	return Handle;
}

bool UUnrealBridgeGeometryLibrary::ReleaseDynamicMesh(int32 Handle)
{
	using namespace BridgeGeometryImpl;
	FHandlePool& Pool = GetPool();
	return Pool.Map.Remove(Handle) > 0;
}

TArray<int32> UUnrealBridgeGeometryLibrary::ListDynamicMeshHandles()
{
	using namespace BridgeGeometryImpl;
	TArray<int32> Out;
	GetPool().Map.GenerateKeyArray(Out);
	Out.Sort();
	return Out;
}

bool UUnrealBridgeGeometryLibrary::LoadMeshFromStaticMesh(int32 Handle, const FString& AssetPath, int32 Lod)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Target = ResolveHandle(Handle);
	if (!Target)
	{
		return false;
	}
	UStaticMesh* Source = LoadObject<UStaticMesh>(nullptr, *AssetPath);
	if (!Source)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: could not load StaticMesh '%s'"), *AssetPath);
		return false;
	}

	FGeometryScriptCopyMeshFromAssetOptions Options;
	FGeometryScriptMeshReadLOD RequestedLOD;
	RequestedLOD.LODType  = EGeometryScriptLODType::SourceModel;
	RequestedLOD.LODIndex = Lod;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(
		Source, Target, Options, RequestedLOD, Outcome,
		/*bUseSectionMaterials=*/true, /*Debug=*/nullptr);

	return Outcome == EGeometryScriptOutcomePins::Success;
}

bool UUnrealBridgeGeometryLibrary::LoadMeshFromComponent(const FString& ActorLabel, const FString& ComponentName, int32 Handle)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Target = ResolveHandle(Handle);
	if (!Target)
	{
		return false;
	}
	UWorld* World = GetEditorWorld();
	AActor* Actor = FindActor(World, ActorLabel);
	if (!Actor)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: actor '%s' not found in editor world"), *ActorLabel);
		return false;
	}
	USceneComponent* Component = FindComponent(Actor, ComponentName);
	if (!Component)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: component '%s' not found on actor '%s'"),
			*ComponentName, *ActorLabel);
		return false;
	}

	FGeometryScriptCopyMeshFromComponentOptions Options;
	FTransform LocalToWorld = FTransform::Identity;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

	UGeometryScriptLibrary_SceneUtilityFunctions::CopyMeshFromComponent(
		Component, Target, Options,
		/*bTransformToWorld=*/false, LocalToWorld, Outcome, /*Debug=*/nullptr);

	return Outcome == EGeometryScriptOutcomePins::Success;
}

FString UUnrealBridgeGeometryLibrary::SaveMeshToNewStaticMesh(int32 Handle, const FString& NewAssetPath, const TArray<UMaterialInterface*>& MaterialList)
{
#if WITH_EDITOR
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Source = ResolveHandle(Handle);
	if (!Source)
	{
		return FString{};
	}

	FGeometryScriptCreateNewStaticMeshAssetOptions Options;
	// Defaults are conservative: don't recompute normals/tangents (caller can use
	// M6-3 explicitly), Nanite off, collision on, default trace flag.
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

	UStaticMesh* NewAsset = UGeometryScriptLibrary_CreateNewAssetFunctions::CreateNewStaticMeshAssetFromMesh(
		Source, NewAssetPath, Options, Outcome, /*Debug=*/nullptr);

	if (Outcome != EGeometryScriptOutcomePins::Success || !NewAsset)
	{
		return FString{};
	}

	// Apply user-supplied materials. CreateNewStaticMeshAssetFromMesh leaves the
	// MaterialSlot list with default material; we overwrite slot N with
	// MaterialList[N] for as many entries as the caller supplied.
	if (MaterialList.Num() > 0)
	{
		TArray<FStaticMaterial>& Slots = NewAsset->GetStaticMaterials();
		for (int32 i = 0; i < MaterialList.Num() && i < Slots.Num(); ++i)
		{
			if (MaterialList[i])
			{
				Slots[i].MaterialInterface = MaterialList[i];
			}
		}
		NewAsset->Modify();
		NewAsset->MarkPackageDirty();
	}

	return NewAsset->GetPathName();
#else
	(void)Handle; (void)NewAssetPath; (void)MaterialList;
	UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: SaveMeshToNewStaticMesh requires WITH_EDITOR"));
	return FString{};
#endif
}

bool UUnrealBridgeGeometryLibrary::SaveMeshToExistingStaticMesh(int32 Handle, const FString& ExistingAssetPath, bool bReplaceMaterials)
{
#if WITH_EDITOR
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Source = ResolveHandle(Handle);
	if (!Source)
	{
		return false;
	}
	UStaticMesh* Target = LoadObject<UStaticMesh>(nullptr, *ExistingAssetPath);
	if (!Target)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: could not load existing StaticMesh '%s'"), *ExistingAssetPath);
		return false;
	}

	FGeometryScriptCopyMeshToAssetOptions Options;
	Options.bReplaceMaterials       = bReplaceMaterials;
	Options.bEmitTransaction        = true;
	Options.bDeferMeshPostEditChange = false;

	FGeometryScriptMeshWriteLOD TargetLOD;  // defaults: SourceModel LOD0
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;

	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(
		Source, Target, Options, TargetLOD, Outcome,
		/*bUseSectionMaterials=*/true, /*Debug=*/nullptr);

	if (Outcome != EGeometryScriptOutcomePins::Success)
	{
		return false;
	}

	Target->Modify();
	Target->MarkPackageDirty();
	return true;
#else
	(void)Handle; (void)ExistingAssetPath; (void)bReplaceMaterials;
	UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: SaveMeshToExistingStaticMesh requires WITH_EDITOR"));
	return false;
#endif
}

FBridgeMeshInfo UUnrealBridgeGeometryLibrary::GetMeshInfo(int32 Handle)
{
	using namespace BridgeGeometryImpl;
	FBridgeMeshInfo Info;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return Info;
	}
	Info.NumTriangles    = Mesh->GetTriangleCount();
	Info.NumVertices     = UGeometryScriptLibrary_MeshQueryFunctions::GetVertexCount(Mesh);
	Info.NumUVLayers     = UGeometryScriptLibrary_MeshQueryFunctions::GetNumUVSets(Mesh);
	Info.bHasNormals     = UGeometryScriptLibrary_MeshQueryFunctions::GetHasTriangleNormals(Mesh);
	Info.bHasVertexColors = UGeometryScriptLibrary_MeshQueryFunctions::GetHasVertexColors(Mesh);
	Info.Bounds          = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
	return Info;
}

bool UUnrealBridgeGeometryLibrary::MeshBoolean(int32 HandleA, int32 HandleB, const FString& Op)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Target = ResolveHandle(HandleA);
	UDynamicMesh* Tool   = ResolveHandle(HandleB);
	if (!Target || !Tool)
	{
		return false;
	}
	EGeometryScriptBooleanOperation Operation;
	if (!ParseBooleanOp(Op, Operation))
	{
		return false;
	}

	FGeometryScriptMeshBooleanOptions Options;  // bFillHoles=true, bSimplifyOutput=true (engine defaults)
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(
		Target, FTransform::Identity,
		Tool,   FTransform::Identity,
		Operation, Options, /*Debug=*/nullptr);

	// ApplyMeshBoolean returns its TargetMesh on success; null/empty result is the failure marker.
	return Result == Target && Target->GetTriangleCount() > 0;
}

bool UUnrealBridgeGeometryLibrary::MeshSmooth(int32 Handle, int32 Iterations, float Strength)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	FGeometryScriptIterativeMeshSmoothingOptions Options;
	Options.NumIterations = FMath::Max(1, Iterations);
	Options.Alpha         = FMath::Clamp(Strength, 0.0f, 1.0f);

	FGeometryScriptMeshSelection EmptySelection;  // empty = whole mesh
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshDeformFunctions::ApplyIterativeSmoothingToMesh(
		Mesh, EmptySelection, Options, /*Debug=*/nullptr);

	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::MeshDecimate(int32 Handle, int32 TargetTris)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	const int32 ClampedTarget = FMath::Max(4, TargetTris);
	FGeometryScriptSimplifyMeshOptions Options;  // AttributeAware default
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshSimplifyFunctions::ApplySimplifyToTriangleCount(
		Mesh, ClampedTarget, Options, /*Debug=*/nullptr);

	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::AppendBox(int32 Handle, FVector Origin, FVector Size)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	FGeometryScriptPrimitiveOptions Options;  // PolyGroupMode::PerFace, etc. — engine defaults.
	const FTransform Xform(Origin);
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendBox(
		Mesh, Options, Xform,
		FMath::Max(0.f, Size.X), FMath::Max(0.f, Size.Y), FMath::Max(0.f, Size.Z),
		/*StepsX=*/0, /*StepsY=*/0, /*StepsZ=*/0,
		EGeometryScriptPrimitiveOriginMode::Center,
		/*Debug=*/nullptr);
	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::AppendSphere(int32 Handle, FVector Origin, float Radius, int32 ResolutionUV)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	FGeometryScriptPrimitiveOptions Options;
	const FTransform Xform(Origin);
	const int32 StepsTheta = FMath::Max(3, ResolutionUV);          // longitude segments
	const int32 StepsPhi   = FMath::Max(2, ResolutionUV / 2);      // latitude rings
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(
		Mesh, Options, Xform,
		FMath::Max(0.f, Radius), StepsPhi, StepsTheta,
		EGeometryScriptPrimitiveOriginMode::Center,
		/*Debug=*/nullptr);
	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::AppendCylinder(int32 Handle, FVector Origin, float Radius, float Height, int32 RadialSegments)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	FGeometryScriptPrimitiveOptions Options;
	const FTransform Xform(Origin);
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCylinder(
		Mesh, Options, Xform,
		FMath::Max(0.f, Radius), FMath::Max(0.f, Height),
		FMath::Max(3, RadialSegments), /*HeightSteps=*/0, /*bCapped=*/true,
		EGeometryScriptPrimitiveOriginMode::Base,
		/*Debug=*/nullptr);
	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::AppendCone(int32 Handle, FVector Origin, float BaseRadius, float Height, int32 RadialSegments)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	FGeometryScriptPrimitiveOptions Options;
	const FTransform Xform(Origin);
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendCone(
		Mesh, Options, Xform,
		FMath::Max(0.f, BaseRadius), /*TopRadius=*/0.f,
		FMath::Max(0.f, Height),
		FMath::Max(3, RadialSegments), /*HeightSteps=*/4,
		/*bCapped=*/true,
		EGeometryScriptPrimitiveOriginMode::Base,
		/*Debug=*/nullptr);
	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::MeshTransform(int32 Handle, FTransform Transform)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshTransformFunctions::TransformMesh(
		Mesh, Transform, /*bFixOrientationForNegativeScale=*/true, /*Debug=*/nullptr);
	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::MeshUniformRemesh(int32 Handle, int32 TargetTriCount)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	FGeometryScriptRemeshOptions          RemeshOptions;       // bReprojectToInputMesh=true, SmoothingRate=0.25 (defaults)
	FGeometryScriptUniformRemeshOptions   UniformOptions;
	UniformOptions.TargetType          = EGeometryScriptUniformRemeshTargetType::TriangleCount;
	UniformOptions.TargetTriangleCount = FMath::Max(4, TargetTriCount);

	UDynamicMesh* Result = UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(
		Mesh, RemeshOptions, UniformOptions, /*Debug=*/nullptr);
	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::MeshDisplaceFromTexture(int32 Handle, const FString& TexturePath, float Magnitude, int32 UVChannel)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}
	UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *TexturePath);
	if (!Tex)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: could not load Texture2D '%s'"), *TexturePath);
		return false;
	}

	FGeometryScriptDisplaceFromTextureOptions Options;
	Options.Magnitude = Magnitude;
	// UVScale, UVOffset, Center, ImageChannel — engine defaults (1, 0, 0.5, 0).

	FGeometryScriptMeshSelection EmptySelection;  // empty = whole mesh
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshDeformFunctions::ApplyDisplaceFromTextureMap(
		Mesh, Tex, EmptySelection, Options, FMath::Max(0, UVChannel), /*Debug=*/nullptr);
	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::MeshVoxelMerge(const TArray<int32>& Handles, float CellSizeCm)
{
	using namespace BridgeGeometryImpl;
	if (Handles.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: MeshVoxelMerge requires ≥1 handle"));
		return false;
	}
	UDynamicMesh* Target = ResolveHandle(Handles[0]);
	if (!Target)
	{
		return false;
	}

	// Append every subsequent handle's geometry into the target (identity transform).
	for (int32 i = 1; i < Handles.Num(); ++i)
	{
		UDynamicMesh* Source = ResolveHandle(Handles[i]);
		if (!Source)
		{
			UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: MeshVoxelMerge skipping invalid handle %d at index %d"), Handles[i], i);
			continue;
		}
		UGeometryScriptLibrary_MeshBasicEditFunctions::AppendMesh(
			Target, Source, FTransform::Identity,
			/*bDeferChangeNotifications=*/true,
			FGeometryScriptAppendMeshOptions(),
			/*Debug=*/nullptr);
	}

	// Solidify the merged result.
	FGeometryScriptSolidifyOptions Options;
	Options.GridParameters.SizeMethod   = EGeometryScriptGridSizingMethod::GridCellSize;
	Options.GridParameters.GridCellSize = FMath::Max(0.001f, CellSizeCm);
	UDynamicMesh* Result = UGeometryScriptLibrary_MeshVoxelFunctions::ApplyMeshSolidify(
		Target, Options, /*Debug=*/nullptr);
	return Result == Target;
}

bool UUnrealBridgeGeometryLibrary::MeshUVUnwrap(int32 Handle, const FString& Method)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}

	const FBox Bounds = UGeometryScriptLibrary_MeshQueryFunctions::GetMeshBoundingBox(Mesh);
	if (!Bounds.IsValid)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge|Geometry: MeshUVUnwrap on empty mesh"));
		return false;
	}

	// Ensure UV channel 0 exists.
	UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets(Mesh, FMath::Max(1, 1), /*Debug=*/nullptr);

	const FVector  Center = Bounds.GetCenter();
	const FVector  Size   = Bounds.GetSize();
	const FTransform PlaneXform(FRotator::ZeroRotator, Center, Size);  // FTransform(rot, loc, scale)
	FGeometryScriptMeshSelection EmptySelection;
	UDynamicMesh* Result = nullptr;
	const FString Lower = Method.ToLower();

	if (Lower == TEXT("box"))
	{
		Result = UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromBoxProjection(
			Mesh, /*UVSetIndex=*/0, PlaneXform, EmptySelection,
			/*MinIslandTriCount=*/2, /*Debug=*/nullptr);
	}
	else if (Lower == TEXT("cylinder"))
	{
		Result = UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromCylinderProjection(
			Mesh, /*UVSetIndex=*/0, PlaneXform, EmptySelection,
			/*SplitAngle=*/45.0f, /*Debug=*/nullptr);
	}
	else if (Lower == TEXT("plane"))
	{
		Result = UGeometryScriptLibrary_MeshUVFunctions::SetMeshUVsFromPlanarProjection(
			Mesh, /*UVSetIndex=*/0, PlaneXform, EmptySelection, /*Debug=*/nullptr);
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge|Geometry: unknown UV unwrap method '%s' (expected box / cylinder / plane)"),
			*Method);
		return false;
	}

	return Result == Mesh;
}

bool UUnrealBridgeGeometryLibrary::RecomputeNormalsAndTangents(int32 Handle, float AngleThresholdDeg)
{
	using namespace BridgeGeometryImpl;
	UDynamicMesh* Mesh = ResolveHandle(Handle);
	if (!Mesh)
	{
		return false;
	}

	FGeometryScriptCalculateNormalsOptions CalcOpts;  // bAngleWeighted=true, bAreaWeighted=true (defaults)
	UDynamicMesh* Result = nullptr;

	if (AngleThresholdDeg > 0.0f)
	{
		FGeometryScriptSplitNormalsOptions SplitOpts;
		SplitOpts.bSplitByOpeningAngle = true;
		SplitOpts.OpeningAngleDeg      = AngleThresholdDeg;
		// Other defaults: bSplitByFaceGroup=true (cheap, harmless when no groups exist).
		Result = UGeometryScriptLibrary_MeshNormalsFunctions::ComputeSplitNormals(
			Mesh, SplitOpts, CalcOpts, /*Debug=*/nullptr);
	}
	else
	{
		Result = UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(
			Mesh, CalcOpts, /*bDeferChangeNotifications=*/false, /*Debug=*/nullptr);
	}

	if (Result != Mesh)
	{
		return false;
	}

	// Tangents always second pass — RecomputeNormals doesn't touch the tangent overlay.
	FGeometryScriptTangentsOptions TangentOpts;  // defaults
	UDynamicMesh* TangResult = UGeometryScriptLibrary_MeshNormalsFunctions::ComputeTangents(
		Mesh, TangentOpts, /*Debug=*/nullptr);
	return TangResult == Mesh;
}

#undef LOCTEXT_NAMESPACE

#endif // !UE_VERSION_OLDER_THAN(5, 7, 0)
