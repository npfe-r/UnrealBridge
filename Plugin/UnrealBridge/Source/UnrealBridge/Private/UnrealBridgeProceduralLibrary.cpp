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
#include "Math/UnrealMathUtility.h"
#include "ScopedTransaction.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"

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

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsOnLandscape(
	const FString& LandscapeLabel, FBox Bounds2D, int32 Count, int32 Seed)
{
	TArray<FVector> Out;

	if (Count <= 0)
	{
		return Out;
	}
	if (Count > BridgeProceduralImpl::MaxPointCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnLandscape Count=%d exceeds cap %d — refusing."),
			Count, BridgeProceduralImpl::MaxPointCount);
		return Out;
	}
	if (!Bounds2D.IsValid)
	{
		return Out;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Target = BridgeProceduralImpl::FindActor(World, LandscapeLabel);
	ALandscapeProxy* RootProxy = Cast<ALandscapeProxy>(Target);
	if (!RootProxy)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnLandscape — '%s' not a Landscape actor"),
			*LandscapeLabel);
		return Out;
	}

	ULandscapeInfo* Info = RootProxy->GetLandscapeInfo();
	if (!Info)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnLandscape — landscape '%s' has no LandscapeInfo "
				 "(check level streaming state)"),
			*LandscapeLabel);
		return Out;
	}

	FRandomStream Rng(Seed);
	Out.Reserve(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		const float X = Rng.FRandRange(Bounds2D.Min.X, Bounds2D.Max.X);
		const float Y = Rng.FRandRange(Bounds2D.Min.Y, Bounds2D.Max.Y);
		const FVector QueryLoc(X, Y, 0.f);

		TOptional<float> ResultZ;
		// ForEachLandscapeProxy iterates root + all streaming proxies; fn returns
		// false to early-exit on first hit (plan §6 #5 — World Partition / streaming
		// proxies cover non-overlapping regions, first hit wins).
		Info->ForEachLandscapeProxy([&ResultZ, &QueryLoc](ALandscapeProxy* P) -> bool
		{
			if (!P)
			{
				return true; // continue
			}
			TOptional<float> R = P->GetHeightAtLocation(QueryLoc);
			if (R.IsSet())
			{
				ResultZ = R;
				return false; // break
			}
			return true; // continue
		});

		if (ResultZ.IsSet())
		{
			Out.Emplace(X, Y, *ResultZ);
		}
	}

	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsPoissonDisk2D(
	FBox Bounds, float MinRadius, int32 MaxAttempts, int32 Seed)
{
	TArray<FVector> Out;

	if (!Bounds.IsValid || MinRadius <= 0.f)
	{
		return Out;
	}
	if (MaxAttempts <= 0)
	{
		MaxAttempts = 30; // Bridson 2007 canonical default
	}

	const FVector Size = Bounds.GetSize();
	if (Size.X <= 0.f || Size.Y <= 0.f)
	{
		// Degenerate bounds — Bridson cell grid would have 0 cells. Plan §6 #4.
		return Out;
	}

	const float OutZ = Bounds.Min.Z;
	const float CellSize = MinRadius / FMath::Sqrt(2.f);
	const int32 GridW = FMath::Max(1, FMath::CeilToInt(Size.X / CellSize));
	const int32 GridH = FMath::Max(1, FMath::CeilToInt(Size.Y / CellSize));

	const int64 GridCells = static_cast<int64>(GridW) * static_cast<int64>(GridH);
	if (GridCells > BridgeProceduralImpl::MaxPointCount * 4)
	{
		// Worst-case grid memory grows with area / R²; for very small R relative
		// to bounds this becomes an OOM hazard. Refuse early.
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsPoissonDisk2D grid (%d × %d = %lld cells) too large; "
				 "raise MinRadius or shrink Bounds."),
			GridW, GridH, GridCells);
		return Out;
	}

	// Grid stores index into Out, INDEX_NONE = empty cell.
	TArray<int32> Grid;
	Grid.Init(INDEX_NONE, static_cast<int32>(GridCells));

	auto CellIndex = [GridW](int32 cx, int32 cy)
	{
		return cy * GridW + cx;
	};

	auto PointToCell = [&](const FVector& P, int32& OutCX, int32& OutCY)
	{
		OutCX = FMath::Clamp(FMath::FloorToInt((P.X - Bounds.Min.X) / CellSize), 0, GridW - 1);
		OutCY = FMath::Clamp(FMath::FloorToInt((P.Y - Bounds.Min.Y) / CellSize), 0, GridH - 1);
	};

	FRandomStream Rng(Seed);
	TArray<int32> Active;
	Out.Reserve(FMath::Min<int32>(static_cast<int32>(GridCells), BridgeProceduralImpl::MaxPointCount));

	// Initial point.
	const FVector First(
		Rng.FRandRange(Bounds.Min.X, Bounds.Max.X),
		Rng.FRandRange(Bounds.Min.Y, Bounds.Max.Y),
		OutZ);
	int32 fcx, fcy;
	PointToCell(First, fcx, fcy);
	Out.Add(First);
	Grid[CellIndex(fcx, fcy)] = 0;
	Active.Add(0);

	const float MinR2 = MinRadius * MinRadius;

	while (Active.Num() > 0)
	{
		if (Out.Num() >= BridgeProceduralImpl::MaxPointCount)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UnrealBridge: SamplePointsPoissonDisk2D hit cap %d, truncating."),
				BridgeProceduralImpl::MaxPointCount);
			break;
		}

		const int32 ActiveIdx = Rng.RandRange(0, Active.Num() - 1);
		const int32 PointIdx = Active[ActiveIdx];
		const FVector P = Out[PointIdx];

		bool bFoundCandidate = false;
		for (int32 attempt = 0; attempt < MaxAttempts; ++attempt)
		{
			const float Angle = Rng.FRandRange(0.f, 2.f * PI);
			const float Dist = Rng.FRandRange(MinRadius, 2.f * MinRadius);
			const float CX = P.X + FMath::Cos(Angle) * Dist;
			const float CY = P.Y + FMath::Sin(Angle) * Dist;

			if (CX < Bounds.Min.X || CX > Bounds.Max.X ||
				CY < Bounds.Min.Y || CY > Bounds.Max.Y)
			{
				continue;
			}

			const FVector Cand(CX, CY, OutZ);
			int32 cx, cy;
			PointToCell(Cand, cx, cy);

			// 5×5 neighborhood: cell size = R/√2 means worst-case neighbor at
			// 2 cells away may still be < R. So search [-2, +2] on both axes.
			bool bConflict = false;
			for (int32 dy = -2; dy <= 2 && !bConflict; ++dy)
			{
				const int32 ny = cy + dy;
				if (ny < 0 || ny >= GridH)
				{
					continue;
				}
				for (int32 dx = -2; dx <= 2 && !bConflict; ++dx)
				{
					const int32 nx = cx + dx;
					if (nx < 0 || nx >= GridW)
					{
						continue;
					}
					const int32 NeighIdx = Grid[CellIndex(nx, ny)];
					if (NeighIdx == INDEX_NONE)
					{
						continue;
					}
					const FVector& Existing = Out[NeighIdx];
					const float ddx = Cand.X - Existing.X;
					const float ddy = Cand.Y - Existing.Y;
					if (ddx * ddx + ddy * ddy < MinR2)
					{
						bConflict = true;
					}
				}
			}

			if (!bConflict)
			{
				const int32 NewIdx = Out.Add(Cand);
				Grid[CellIndex(cx, cy)] = NewIdx;
				Active.Add(NewIdx);
				bFoundCandidate = true;
				break;
			}
		}

		if (!bFoundCandidate)
		{
			Active.RemoveAtSwap(ActiveIdx);
		}
	}

	return Out;
}

// ─── M2 — Filter ─────────────────────────────────────────────

TArray<FVector> UUnrealBridgeProceduralLibrary::ProjectPointsToSurface(
	const TArray<FVector>& In, float BounceUp, float BounceDown, TArray<FVector>& OutHitNormals)
{
	TArray<FVector> Out;
	OutHitNormals.Reset();

	if (In.Num() == 0)
	{
		return Out;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	if (!World)
	{
		// Degenerate: return inputs unchanged with up-normals so caller's pipeline
		// stays parallel-safe (plan D2 — same length in/out).
		Out = In;
		OutHitNormals.Init(FVector::UpVector, In.Num());
		return Out;
	}

	Out.Reserve(In.Num());
	OutHitNormals.Reserve(In.Num());

	FCollisionQueryParams Params(SCENE_QUERY_STAT(BridgeProjectToSurface), /*bTraceComplex=*/true);

	for (const FVector& P : In)
	{
		FHitResult Hit;
		const FVector Start(P.X, P.Y, P.Z + BounceUp);
		const FVector End(P.X, P.Y, P.Z - BounceDown);

		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
		{
			Out.Add(Hit.ImpactPoint);
			OutHitNormals.Add(Hit.ImpactNormal);
		}
		else
		{
			// Plan D2 — keep the slot, pass through original + degenerate up-normal.
			Out.Add(P);
			OutHitNormals.Add(FVector::UpVector);
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
