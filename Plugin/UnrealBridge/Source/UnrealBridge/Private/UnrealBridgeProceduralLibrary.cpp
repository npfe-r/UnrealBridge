#include "UnrealBridgeProceduralLibrary.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/HitResult.h"
#include "Engine/EngineTypes.h"
#include "EngineUtils.h"                     // TActorIterator
#include "Editor.h"                          // GEditor
#include "Editor/EditorEngine.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "CollisionQueryParams.h"
#include "Math/RandomStream.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "UnrealBridgeProcedural"

// Hard cap shared by all sampling functions — see class docblock.
// Above this, return empty + log; that's PCG's territory, not ours.
namespace BridgeProceduralImpl
{
	constexpr int32 MaxPointCount = 100000;

	UWorld* GetEditorWorld()
	{
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return nullptr;
	}

	/** Match by FName or user-visible label, case-sensitive. Mirrors the resolver in
	 *  UnrealBridgeLevelLibrary so callers can pass either form interchangeably. */
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

	/** First matching actor that carries `Tag`. Used by EnsureProceduralISMActor to
	 *  reuse the existing stub before spawning a new one. */
	AActor* FindFirstActorWithTag(UWorld* World, const FName& Tag)
	{
		if (!World || Tag.IsNone())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (A && A->Tags.Contains(Tag))
			{
				return A;
			}
		}
		return nullptr;
	}

	/** Returns the ISM/HISM component owned by the actor, regardless of which subclass
	 *  EnsureProceduralISMActor created — HISM derives from ISM, so a single
	 *  TInlineComponentArray<UInstancedStaticMeshComponent*> picks up both. */
	UInstancedStaticMeshComponent* FindISMComponent(AActor* Actor)
	{
		if (!Actor)
		{
			return nullptr;
		}
		TInlineComponentArray<UInstancedStaticMeshComponent*> Comps;
		Actor->GetComponents(Comps);
		return Comps.Num() > 0 ? Comps[0] : nullptr;
	}
}

// ─── M1 — Sampling ───────────────────────────────────────────

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsGrid(
	FBox Bounds, float Spacing, float JitterRatio, int32 Seed)
{
	TArray<FVector> Out;

	if (!Bounds.IsValid || Spacing <= 0.f)
	{
		return Out;
	}

	const FVector BoundsSize = Bounds.GetSize();
	if (BoundsSize.X <= 0.f || BoundsSize.Y <= 0.f)
	{
		return Out;
	}

	// Cells are floor(size/spacing). +1 to include the upper edge anchor when
	// size is an exact multiple of spacing — otherwise corners are missed.
	const int32 NumX = FMath::Max(1, FMath::FloorToInt(BoundsSize.X / Spacing) + 1);
	const int32 NumY = FMath::Max(1, FMath::FloorToInt(BoundsSize.Y / Spacing) + 1);

	const int64 Total = static_cast<int64>(NumX) * static_cast<int64>(NumY);
	if (Total > BridgeProceduralImpl::MaxPointCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsGrid would yield %lld points (cap %d) — refusing. "
				 "Increase Spacing or shrink Bounds."),
			Total, BridgeProceduralImpl::MaxPointCount);
		return Out;
	}

	const float JitterClamp = FMath::Clamp(JitterRatio, 0.f, 0.5f);
	const float JitterSpan = JitterClamp * Spacing;
	const float Z = (Bounds.Min.Z + Bounds.Max.Z) * 0.5f;

	FRandomStream Rng(Seed);
	Out.Reserve(static_cast<int32>(Total));

	for (int32 ix = 0; ix < NumX; ++ix)
	{
		const float BaseX = Bounds.Min.X + static_cast<float>(ix) * Spacing;
		for (int32 iy = 0; iy < NumY; ++iy)
		{
			const float BaseY = Bounds.Min.Y + static_cast<float>(iy) * Spacing;
			const float JX = JitterSpan > 0.f ? Rng.FRandRange(-JitterSpan, JitterSpan) : 0.f;
			const float JY = JitterSpan > 0.f ? Rng.FRandRange(-JitterSpan, JitterSpan) : 0.f;
			Out.Emplace(BaseX + JX, BaseY + JY, Z);
		}
	}

	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsOnSurface(
	const FString& ActorLabel, int32 Count, int32 Seed, float MaxBounceUp)
{
	TArray<FVector> Out;

	if (Count <= 0)
	{
		return Out;
	}
	if (Count > BridgeProceduralImpl::MaxPointCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnSurface Count=%d exceeds cap %d — refusing."),
			Count, BridgeProceduralImpl::MaxPointCount);
		return Out;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Target = BridgeProceduralImpl::FindActor(World, ActorLabel);
	if (!Target)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnSurface — actor '%s' not found"), *ActorLabel);
		return Out;
	}

	FVector Origin, Extent;
	Target->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent,
		/*bIncludeFromChildActors=*/true);

	const FBox Bounds = FBox::BuildAABB(Origin, Extent);
	const float ZTop = Bounds.Max.Z + MaxBounceUp;
	const float ZBot = Bounds.Min.Z - MaxBounceUp;

	FRandomStream Rng(Seed);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BridgeSampleOnSurface), /*bTraceComplex=*/true);

	Out.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		const float X = Rng.FRandRange(Bounds.Min.X, Bounds.Max.X);
		const float Y = Rng.FRandRange(Bounds.Min.Y, Bounds.Max.Y);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, FVector(X, Y, ZTop), FVector(X, Y, ZBot),
				ECC_Visibility, Params))
		{
			Out.Add(Hit.ImpactPoint);
		}
	}

	return Out;
}

// ─── M3 — Instancing ─────────────────────────────────────────

FString UUnrealBridgeProceduralLibrary::EnsureProceduralISMActor(
	const FString& Tag, const FString& MeshPath, bool bUseHISM)
{
	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	if (!World)
	{
		return FString();
	}

	const FName TagName(*Tag);
	if (TagName.IsNone())
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge: EnsureProceduralISMActor — empty Tag refused"));
		return FString();
	}

	// Reuse the existing stub if its [H]ISMC's mesh already matches; otherwise
	// return early on tag hit so the caller can decide (we don't silently swap meshes).
	if (AActor* Existing = BridgeProceduralImpl::FindFirstActorWithTag(World, TagName))
	{
		return Existing->GetActorLabel();
	}

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
	if (!Mesh)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: EnsureProceduralISMActor — mesh '%s' failed to load"), *MeshPath);
		return FString();
	}

	FScopedTransaction Tr(LOCTEXT("EnsureProceduralISMActor", "Ensure Procedural ISM Actor"));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* New = World->SpawnActor<AActor>(AActor::StaticClass(),
		FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (!New)
	{
		UE_LOG(LogTemp, Warning, TEXT("UnrealBridge: EnsureProceduralISMActor — SpawnActor failed"));
		return FString();
	}
	New->Modify();
	New->Tags.Add(TagName);
	New->SetActorLabel(FString::Printf(TEXT("Procedural_%s"), *Tag));

	UInstancedStaticMeshComponent* Comp = bUseHISM
		? NewObject<UHierarchicalInstancedStaticMeshComponent>(New, NAME_None, RF_Transactional)
		: NewObject<UInstancedStaticMeshComponent>(New, NAME_None, RF_Transactional);
	if (!Comp)
	{
		World->EditorDestroyActor(New, /*bShouldModifyLevel=*/true);
		return FString();
	}

	Comp->SetStaticMesh(Mesh);
	New->SetRootComponent(Comp);
	Comp->RegisterComponent();
	New->AddInstanceComponent(Comp);

	return New->GetActorLabel();
}

TArray<int32> UUnrealBridgeProceduralLibrary::AddInstancesByTransforms(
	const FString& ActorName, const TArray<FTransform>& Xs, bool bWorldSpace)
{
	TArray<int32> Out;

	if (Xs.Num() == 0)
	{
		return Out;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, ActorName);
	if (!Actor)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: AddInstancesByTransforms — actor '%s' not found"), *ActorName);
		return Out;
	}

	UInstancedStaticMeshComponent* ISMC = BridgeProceduralImpl::FindISMComponent(Actor);
	if (!ISMC)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: AddInstancesByTransforms — actor '%s' has no [H]ISMC"), *ActorName);
		return Out;
	}

	FScopedTransaction Tr(LOCTEXT("AddInstancesByTransforms", "Add Instances By Transforms"));
	Actor->Modify();
	ISMC->Modify();

	// bShouldReturnIndices=true is the whole point — the default is false (Engine
	// optimisation) and you can't update/remove these instances later without it.
	// bUpdateNavigation=false defers nav rebuild to a single end-of-batch
	// RebuildProceduralNavigation call (M3-6) — see plan §6 #2.
	Out = ISMC->AddInstances(Xs, /*bShouldReturnIndices=*/true,
		bWorldSpace, /*bUpdateNavigation=*/false);

	return Out;
}

bool UUnrealBridgeProceduralLibrary::ClearInstances(const FString& ActorName)
{
	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, ActorName);
	if (!Actor)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: ClearInstances — actor '%s' not found"), *ActorName);
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = BridgeProceduralImpl::FindISMComponent(Actor);
	if (!ISMC)
	{
		return false;
	}

	FScopedTransaction Tr(LOCTEXT("ClearInstances", "Clear Procedural Instances"));
	Actor->Modify();
	ISMC->Modify();
	ISMC->ClearInstances();
	ISMC->MarkRenderStateDirty();
	return true;
}

#undef LOCTEXT_NAMESPACE
