#include "UnrealBridgeProceduralLibrary.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
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
#include "LandscapeComponent.h"
#include "NavigationSystem.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Volume.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"

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

	/** Resolve a USplineComponent by name, or first one if Name is empty. */
	USplineComponent* FindSplineComponent(AActor* Actor, const FString& ComponentName)
	{
		if (!Actor)
		{
			return nullptr;
		}
		TInlineComponentArray<USplineComponent*> Comps;
		Actor->GetComponents(Comps);
		if (ComponentName.IsEmpty())
		{
			return Comps.Num() > 0 ? Comps[0] : nullptr;
		}
		for (USplineComponent* C : Comps)
		{
			if (C && C->GetName() == ComponentName)
			{
				return C;
			}
		}
		return nullptr;
	}

	/** Volume-aware containment test. AVolume uses brush geometry; everything
	 *  else falls back to actor world bounds (loose AABB). */
	bool IsPointInsideActor(AActor* Actor, const FVector& Point)
	{
		if (!Actor)
		{
			return false;
		}
		if (AVolume* Vol = Cast<AVolume>(Actor))
		{
			return Vol->EncompassesPoint(Point);
		}
		FVector Origin, Extent;
		Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent,
			/*bIncludeFromChildActors=*/true);
		const FBox Bounds(Origin - Extent, Origin + Extent);
		return Bounds.IsInsideOrOn(Point);
	}

	/** Box-Muller standard normal sample. Cheap, "natural enough" for jitter. */
	float Gauss(FRandomStream& Rng)
	{
		const float U1 = FMath::Max(Rng.GetFraction(), KINDA_SMALL_NUMBER);
		const float U2 = Rng.GetFraction();
		return FMath::Sqrt(-2.f * FMath::Loge(U1)) * FMath::Cos(2.f * PI * U2);
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

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsPoissonDisk3D(
	FBox Bounds, float MinRadius, int32 MaxAttempts, int32 Seed)
{
	TArray<FVector> Out;

	if (!Bounds.IsValid || MinRadius <= 0.f)
	{
		return Out;
	}
	if (MaxAttempts <= 0)
	{
		MaxAttempts = 30;
	}

	const FVector Size = Bounds.GetSize();
	if (Size.X <= 0.f || Size.Y <= 0.f || Size.Z <= 0.f)
	{
		return Out;
	}

	// 3D Bridson: cell = R/√3 means worst-case in-cell dist is R; 5×5×5 scan
	// because two cells away can still fall within R after diagonal.
	const float CellSize = MinRadius / FMath::Sqrt(3.f);
	const int32 GridX = FMath::Max(1, FMath::CeilToInt(Size.X / CellSize));
	const int32 GridY = FMath::Max(1, FMath::CeilToInt(Size.Y / CellSize));
	const int32 GridZ = FMath::Max(1, FMath::CeilToInt(Size.Z / CellSize));

	const int64 Cells = static_cast<int64>(GridX) * GridY * GridZ;
	if (Cells > static_cast<int64>(BridgeProceduralImpl::MaxPointCount) * 4)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsPoissonDisk3D grid (%d × %d × %d = %lld cells) "
				 "too large; raise MinRadius or shrink Bounds."),
			GridX, GridY, GridZ, Cells);
		return Out;
	}

	TArray<int32> Grid;
	Grid.Init(INDEX_NONE, static_cast<int32>(Cells));

	auto CellIndex = [GridX, GridY](int32 cx, int32 cy, int32 cz)
	{
		return (cz * GridY + cy) * GridX + cx;
	};
	auto PointToCell = [&](const FVector& P, int32& cx, int32& cy, int32& cz)
	{
		cx = FMath::Clamp(FMath::FloorToInt((P.X - Bounds.Min.X) / CellSize), 0, GridX - 1);
		cy = FMath::Clamp(FMath::FloorToInt((P.Y - Bounds.Min.Y) / CellSize), 0, GridY - 1);
		cz = FMath::Clamp(FMath::FloorToInt((P.Z - Bounds.Min.Z) / CellSize), 0, GridZ - 1);
	};

	FRandomStream Rng(Seed);
	TArray<int32> Active;
	Out.Reserve(FMath::Min<int32>(static_cast<int32>(Cells), BridgeProceduralImpl::MaxPointCount));

	const FVector First(
		Rng.FRandRange(Bounds.Min.X, Bounds.Max.X),
		Rng.FRandRange(Bounds.Min.Y, Bounds.Max.Y),
		Rng.FRandRange(Bounds.Min.Z, Bounds.Max.Z));
	int32 fx, fy, fz;
	PointToCell(First, fx, fy, fz);
	Out.Add(First);
	Grid[CellIndex(fx, fy, fz)] = 0;
	Active.Add(0);

	const float MinR2 = MinRadius * MinRadius;

	while (Active.Num() > 0)
	{
		if (Out.Num() >= BridgeProceduralImpl::MaxPointCount)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UnrealBridge: SamplePointsPoissonDisk3D hit cap %d, truncating."),
				BridgeProceduralImpl::MaxPointCount);
			break;
		}

		const int32 ActiveIdx = Rng.RandRange(0, Active.Num() - 1);
		const FVector P = Out[Active[ActiveIdx]];

		bool bFound = false;
		for (int32 attempt = 0; attempt < MaxAttempts; ++attempt)
		{
			// Uniform direction on a sphere via spherical coords.
			const float Phi = Rng.FRandRange(0.f, 2.f * PI);
			const float CosTheta = Rng.FRandRange(-1.f, 1.f);
			const float SinTheta = FMath::Sqrt(FMath::Max(0.f, 1.f - CosTheta * CosTheta));
			const float Dist = Rng.FRandRange(MinRadius, 2.f * MinRadius);

			const FVector Cand(
				P.X + Dist * SinTheta * FMath::Cos(Phi),
				P.Y + Dist * SinTheta * FMath::Sin(Phi),
				P.Z + Dist * CosTheta);

			if (Cand.X < Bounds.Min.X || Cand.X > Bounds.Max.X ||
				Cand.Y < Bounds.Min.Y || Cand.Y > Bounds.Max.Y ||
				Cand.Z < Bounds.Min.Z || Cand.Z > Bounds.Max.Z)
			{
				continue;
			}

			int32 cx, cy, cz;
			PointToCell(Cand, cx, cy, cz);

			bool bConflict = false;
			for (int32 dz = -2; dz <= 2 && !bConflict; ++dz)
			{
				const int32 nz = cz + dz;
				if (nz < 0 || nz >= GridZ) continue;
				for (int32 dy = -2; dy <= 2 && !bConflict; ++dy)
				{
					const int32 ny = cy + dy;
					if (ny < 0 || ny >= GridY) continue;
					for (int32 dx = -2; dx <= 2 && !bConflict; ++dx)
					{
						const int32 nx = cx + dx;
						if (nx < 0 || nx >= GridX) continue;
						const int32 NeighIdx = Grid[CellIndex(nx, ny, nz)];
						if (NeighIdx == INDEX_NONE) continue;
						const FVector& Q = Out[NeighIdx];
						const float D2 = FVector::DistSquared(Cand, Q);
						if (D2 < MinR2)
						{
							bConflict = true;
						}
					}
				}
			}

			if (!bConflict)
			{
				const int32 NewIdx = Out.Add(Cand);
				Grid[CellIndex(cx, cy, cz)] = NewIdx;
				Active.Add(NewIdx);
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			Active.RemoveAtSwap(ActiveIdx);
		}
	}

	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsOnSpline(
	const FString& SplineActorLabel, const FString& ComponentName,
	const FString& Mode, float CountOrSpacing)
{
	TArray<FVector> Out;

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, SplineActorLabel);
	USplineComponent* Spline = BridgeProceduralImpl::FindSplineComponent(Actor, ComponentName);
	if (!Spline)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnSpline — spline '%s.%s' not found"),
			*SplineActorLabel, *ComponentName);
		return Out;
	}

	const float Length = Spline->GetSplineLength();
	if (Length <= 0.f)
	{
		return Out;
	}

	int32 N = 0;
	float Step = 0.f;
	if (Mode == TEXT("by_count"))
	{
		N = FMath::Max(1, FMath::FloorToInt(CountOrSpacing));
		Step = (N > 1) ? Length / static_cast<float>(N - 1) : 0.f;
	}
	else if (Mode == TEXT("by_distance"))
	{
		Step = FMath::Max(KINDA_SMALL_NUMBER, CountOrSpacing);
		N = FMath::Max(1, FMath::FloorToInt(Length / Step) + 1);
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnSpline — bad Mode '%s' (expected 'by_count' or 'by_distance')"),
			*Mode);
		return Out;
	}

	if (N > BridgeProceduralImpl::MaxPointCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsOnSpline N=%d exceeds cap %d — refusing."),
			N, BridgeProceduralImpl::MaxPointCount);
		return Out;
	}

	Out.Reserve(N);
	for (int32 i = 0; i < N; ++i)
	{
		const float D = (Mode == TEXT("by_count") && N > 1)
			? static_cast<float>(i) * Step
			: FMath::Min(Length, static_cast<float>(i) * Step);
		Out.Add(Spline->GetLocationAtDistanceAlongSpline(D, ESplineCoordinateSpace::World));
	}
	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsInVolume(
	const FString& VolumeActorLabel, int32 Count, int32 Seed, int32 MaxAttempts)
{
	TArray<FVector> Out;

	if (Count <= 0)
	{
		return Out;
	}
	if (Count > BridgeProceduralImpl::MaxPointCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsInVolume Count=%d exceeds cap %d — refusing."),
			Count, BridgeProceduralImpl::MaxPointCount);
		return Out;
	}
	if (MaxAttempts <= 0)
	{
		MaxAttempts = 30;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, VolumeActorLabel);
	if (!Actor)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsInVolume — actor '%s' not found"), *VolumeActorLabel);
		return Out;
	}

	FVector Origin, Extent;
	Actor->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent,
		/*bIncludeFromChildActors=*/true);
	const FBox AABB = FBox::BuildAABB(Origin, Extent);

	FRandomStream Rng(Seed);
	Out.Reserve(Count);

	const int32 BudgetTotal = Count * MaxAttempts;
	for (int32 attempt = 0; attempt < BudgetTotal && Out.Num() < Count; ++attempt)
	{
		const FVector P(
			Rng.FRandRange(AABB.Min.X, AABB.Max.X),
			Rng.FRandRange(AABB.Min.Y, AABB.Max.Y),
			Rng.FRandRange(AABB.Min.Z, AABB.Max.Z));
		if (BridgeProceduralImpl::IsPointInsideActor(Actor, P))
		{
			Out.Add(P);
		}
	}
	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::SamplePointsJitterStratified(
	FBox Bounds, int32 GridResolution, int32 Seed)
{
	TArray<FVector> Out;
	if (!Bounds.IsValid || GridResolution <= 0)
	{
		return Out;
	}
	const int64 Total = static_cast<int64>(GridResolution) * GridResolution;
	if (Total > BridgeProceduralImpl::MaxPointCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SamplePointsJitterStratified %lld points exceeds cap %d."),
			Total, BridgeProceduralImpl::MaxPointCount);
		return Out;
	}

	const FVector Size = Bounds.GetSize();
	if (Size.X <= 0.f || Size.Y <= 0.f)
	{
		return Out;
	}
	const float CellX = Size.X / static_cast<float>(GridResolution);
	const float CellY = Size.Y / static_cast<float>(GridResolution);
	const float Z = (Bounds.Min.Z + Bounds.Max.Z) * 0.5f;

	FRandomStream Rng(Seed);
	Out.Reserve(static_cast<int32>(Total));

	for (int32 ix = 0; ix < GridResolution; ++ix)
	{
		const float X0 = Bounds.Min.X + ix * CellX;
		for (int32 iy = 0; iy < GridResolution; ++iy)
		{
			const float Y0 = Bounds.Min.Y + iy * CellY;
			Out.Emplace(
				X0 + Rng.FRandRange(0.f, CellX),
				Y0 + Rng.FRandRange(0.f, CellY),
				Z);
		}
	}
	return Out;
}

TArray<FTransform> UUnrealBridgeProceduralLibrary::SampleTransformsAlongSpline(
	const FString& SplineActorLabel, const FString& ComponentName,
	const FString& Mode, float CountOrSpacing)
{
	TArray<FTransform> Out;

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, SplineActorLabel);
	USplineComponent* Spline = BridgeProceduralImpl::FindSplineComponent(Actor, ComponentName);
	if (!Spline)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SampleTransformsAlongSpline — spline '%s.%s' not found"),
			*SplineActorLabel, *ComponentName);
		return Out;
	}

	const float Length = Spline->GetSplineLength();
	if (Length <= 0.f)
	{
		return Out;
	}

	int32 N = 0;
	float Step = 0.f;
	if (Mode == TEXT("by_count"))
	{
		N = FMath::Max(1, FMath::FloorToInt(CountOrSpacing));
		Step = (N > 1) ? Length / static_cast<float>(N - 1) : 0.f;
	}
	else if (Mode == TEXT("by_distance"))
	{
		Step = FMath::Max(KINDA_SMALL_NUMBER, CountOrSpacing);
		N = FMath::Max(1, FMath::FloorToInt(Length / Step) + 1);
	}
	else
	{
		return Out;
	}

	if (N > BridgeProceduralImpl::MaxPointCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: SampleTransformsAlongSpline N=%d exceeds cap %d."),
			N, BridgeProceduralImpl::MaxPointCount);
		return Out;
	}

	Out.Reserve(N);
	for (int32 i = 0; i < N; ++i)
	{
		const float D = (Mode == TEXT("by_count") && N > 1)
			? static_cast<float>(i) * Step
			: FMath::Min(Length, static_cast<float>(i) * Step);
		const FVector Loc = Spline->GetLocationAtDistanceAlongSpline(D, ESplineCoordinateSpace::World);
		const FVector Tan = Spline->GetTangentAtDistanceAlongSpline(D, ESplineCoordinateSpace::World);
		const FRotator Rot = Tan.IsNearlyZero() ? FRotator::ZeroRotator : Tan.Rotation();
		Out.Emplace(Rot, Loc, FVector::OneVector);
	}
	return Out;
}

TArray<FTransform> UUnrealBridgeProceduralLibrary::JitterTransforms(
	const TArray<FTransform>& Xs, float PosSigma, float RotSigma,
	FVector ScaleMin, FVector ScaleMax, int32 Seed)
{
	TArray<FTransform> Out;
	Out.Reserve(Xs.Num());

	FRandomStream Rng(Seed);
	for (const FTransform& X : Xs)
	{
		const FVector Pos = X.GetLocation()
			+ FVector(BridgeProceduralImpl::Gauss(Rng) * PosSigma,
			          BridgeProceduralImpl::Gauss(Rng) * PosSigma,
			          BridgeProceduralImpl::Gauss(Rng) * PosSigma);

		FRotator R = X.Rotator();
		R.Pitch += BridgeProceduralImpl::Gauss(Rng) * RotSigma;
		R.Yaw   += BridgeProceduralImpl::Gauss(Rng) * RotSigma;
		R.Roll  += BridgeProceduralImpl::Gauss(Rng) * RotSigma;

		const FVector S(
			Rng.FRandRange(ScaleMin.X, ScaleMax.X),
			Rng.FRandRange(ScaleMin.Y, ScaleMax.Y),
			Rng.FRandRange(ScaleMin.Z, ScaleMax.Z));
		const FVector NewScale = X.GetScale3D() * S;

		Out.Emplace(R, Pos, NewScale);
	}
	return Out;
}

// ─── M2 — Filter ─────────────────────────────────────────────

TArray<FVector> UUnrealBridgeProceduralLibrary::FilterPointsBySlope(
	const TArray<FVector>& In, float MaxSlopeDeg, float BounceUp)
{
	TArray<FVector> Out;

	if (In.Num() == 0)
	{
		return Out;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	if (!World)
	{
		return Out;
	}

	// Slope angle is between hit normal and +Z. Keep when dot >= cos(MaxSlope).
	const float MaxRad = FMath::DegreesToRadians(FMath::Clamp(MaxSlopeDeg, 0.f, 90.f));
	const float MinDot = FMath::Cos(MaxRad);

	Out.Reserve(In.Num());
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BridgeFilterBySlope), /*bTraceComplex=*/true);

	for (const FVector& P : In)
	{
		FHitResult Hit;
		const FVector Start(P.X, P.Y, P.Z + BounceUp);
		const FVector End(P.X, P.Y, P.Z - BounceUp);

		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
		{
			const float Dot = FVector::DotProduct(Hit.ImpactNormal, FVector::UpVector);
			if (Dot >= MinDot)
			{
				Out.Add(P);
			}
		}
		// miss → drop (no surface = can't determine slope)
	}

	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::FilterPointsByMinDistance(
	const TArray<FVector>& In, float MinDist)
{
	if (In.Num() == 0)
	{
		return TArray<FVector>();
	}
	if (MinDist <= 0.f)
	{
		// Pass-through — no filter.
		return In;
	}

	// Compute XY bounds of input set.
	float MinX = TNumericLimits<float>::Max();
	float MinY = TNumericLimits<float>::Max();
	float MaxX = TNumericLimits<float>::Lowest();
	float MaxY = TNumericLimits<float>::Lowest();
	for (const FVector& P : In)
	{
		MinX = FMath::Min(MinX, static_cast<float>(P.X));
		MaxX = FMath::Max(MaxX, static_cast<float>(P.X));
		MinY = FMath::Min(MinY, static_cast<float>(P.Y));
		MaxY = FMath::Max(MaxY, static_cast<float>(P.Y));
	}

	const float CellSize = MinDist / FMath::Sqrt(2.f);
	const int32 GridW = FMath::Max(1, FMath::CeilToInt((MaxX - MinX) / CellSize) + 1);
	const int32 GridH = FMath::Max(1, FMath::CeilToInt((MaxY - MinY) / CellSize) + 1);

	const int64 Cells = static_cast<int64>(GridW) * static_cast<int64>(GridH);
	if (Cells > static_cast<int64>(BridgeProceduralImpl::MaxPointCount) * 10)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: FilterPointsByMinDistance grid (%d × %d = %lld cells) too large; "
				 "raise MinDist or shrink input span."),
			GridW, GridH, Cells);
		return TArray<FVector>();
	}

	TArray<TArray<int32>> Grid;
	Grid.SetNum(static_cast<int32>(Cells));

	TArray<FVector> Out;
	Out.Reserve(In.Num());
	const float MinD2 = MinDist * MinDist;

	for (const FVector& P : In)
	{
		const int32 cx = FMath::Clamp(FMath::FloorToInt((P.X - MinX) / CellSize), 0, GridW - 1);
		const int32 cy = FMath::Clamp(FMath::FloorToInt((P.Y - MinY) / CellSize), 0, GridH - 1);

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
				const TArray<int32>& Cell = Grid[ny * GridW + nx];
				for (int32 NeighIdx : Cell)
				{
					const FVector& Q = Out[NeighIdx];
					const float ddx = static_cast<float>(P.X - Q.X);
					const float ddy = static_cast<float>(P.Y - Q.Y);
					if (ddx * ddx + ddy * ddy < MinD2)
					{
						bConflict = true;
						break;
					}
				}
			}
		}

		if (!bConflict)
		{
			const int32 NewIdx = Out.Add(P);
			Grid[cy * GridW + cx].Add(NewIdx);
		}
	}

	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::FilterPointsByOverlap(
	const TArray<FVector>& Pts, const TArray<FString>& BlockingClassPaths, float Radius)
{
	TArray<FVector> Out;
	if (Pts.Num() == 0)
	{
		return Out;
	}
	if (Radius <= 0.f)
	{
		// No overlap test possible — pass-through.
		return Pts;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	if (!World)
	{
		return Out;
	}

	// Resolve blocking classes once. Bad entries silently skipped.
	TArray<UClass*> BlockingClasses;
	BlockingClasses.Reserve(BlockingClassPaths.Num());
	for (const FString& Path : BlockingClassPaths)
	{
		if (Path.IsEmpty()) continue;
		UClass* Cls = LoadObject<UClass>(nullptr, *Path);
		if (!Cls)
		{
			Cls = LoadObject<UClass>(nullptr, *(Path + TEXT("_C")));
		}
		if (Cls)
		{
			BlockingClasses.Add(Cls);
		}
	}

	if (BlockingClasses.Num() == 0)
	{
		// Nothing to block against → pass-through.
		return Pts;
	}

	Out.Reserve(Pts.Num());
	const FCollisionShape Sphere = FCollisionShape::MakeSphere(Radius);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(BridgeFilterByOverlap), /*bTraceComplex=*/false);

	for (const FVector& P : Pts)
	{
		TArray<FOverlapResult> Hits;
		World->OverlapMultiByObjectType(Hits, P, FQuat::Identity,
			FCollisionObjectQueryParams::AllObjects, Sphere, Params);

		bool bBlocked = false;
		for (const FOverlapResult& H : Hits)
		{
			AActor* HitActor = H.GetActor();
			if (!HitActor) continue;
			for (UClass* Cls : BlockingClasses)
			{
				if (HitActor->IsA(Cls))
				{
					bBlocked = true;
					break;
				}
			}
			if (bBlocked) break;
		}
		if (!bBlocked)
		{
			Out.Add(P);
		}
	}
	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::FilterPointsByDensityMask(
	const TArray<FVector>& Pts, const FString& TextureAsset, FBox BoundsXY,
	int32 ChannelIndex, float Threshold, int32 Seed)
{
	TArray<FVector> Out;
	if (Pts.Num() == 0)
	{
		return Out;
	}

	UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, *TextureAsset);
	if (!Tex)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: FilterPointsByDensityMask — texture '%s' failed to load"),
			*TextureAsset);
		return Out;
	}

	// Per plan §6 #11: TC_VectorDisplacementmap is the one compression mode that
	// keeps raw RGBA8 readable. DXT/BC compressed textures return GPU-only data.
	if (Tex->CompressionSettings != TC_VectorDisplacementmap)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: FilterPointsByDensityMask — texture '%s' has CompressionSettings=%d "
				 "(need TC_VectorDisplacementmap=%d for raw RGBA8 read). Set this in the texture "
				 "asset and reimport."),
			*TextureAsset, static_cast<int32>(Tex->CompressionSettings),
			static_cast<int32>(TC_VectorDisplacementmap));
		return Out;
	}

	FTexturePlatformData* PlatformData = Tex->GetPlatformData();
	if (!PlatformData || PlatformData->Mips.Num() == 0)
	{
		return Out;
	}
	FTexture2DMipMap& Mip0 = PlatformData->Mips[0];
	const int32 W = Mip0.SizeX;
	const int32 H = Mip0.SizeY;
	if (W <= 0 || H <= 0)
	{
		return Out;
	}

	const FColor* Pixels = static_cast<const FColor*>(Mip0.BulkData.LockReadOnly());
	if (!Pixels)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: FilterPointsByDensityMask — mip 0 BulkData lock failed for '%s'"),
			*TextureAsset);
		return Out;
	}

	const float SpanX = FMath::Max(BoundsXY.Max.X - BoundsXY.Min.X, KINDA_SMALL_NUMBER);
	const float SpanY = FMath::Max(BoundsXY.Max.Y - BoundsXY.Min.Y, KINDA_SMALL_NUMBER);

	FRandomStream Rng(Seed);
	Out.Reserve(Pts.Num());

	const int32 SafeChannel = FMath::Clamp(ChannelIndex, 0, 3);
	const float SafeThreshold = FMath::Clamp(Threshold, 0.f, 1.f);

	for (const FVector& P : Pts)
	{
		const float U = (P.X - BoundsXY.Min.X) / SpanX;
		const float V = (P.Y - BoundsXY.Min.Y) / SpanY;
		if (U < 0.f || U > 1.f || V < 0.f || V > 1.f)
		{
			continue;
		}
		const int32 PX = FMath::Clamp(FMath::FloorToInt(U * W), 0, W - 1);
		const int32 PY = FMath::Clamp(FMath::FloorToInt(V * H), 0, H - 1);
		const FColor C = Pixels[PY * W + PX];

		float Channel = 0.f;
		switch (SafeChannel)
		{
			case 0: Channel = C.R / 255.f; break;
			case 1: Channel = C.G / 255.f; break;
			case 2: Channel = C.B / 255.f; break;
			case 3: Channel = C.A / 255.f; break;
		}

		// Hard cutoff first, then stochastic.
		if (Channel < SafeThreshold) continue;
		if (Rng.FRand() < Channel)
		{
			Out.Add(P);
		}
	}

	Mip0.BulkData.Unlock();
	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::FilterPointsInsideActor(
	const TArray<FVector>& Pts, const FString& ContainerActorLabel, bool bInside)
{
	TArray<FVector> Out;
	if (Pts.Num() == 0)
	{
		return Out;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Container = BridgeProceduralImpl::FindActor(World, ContainerActorLabel);
	if (!Container)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: FilterPointsInsideActor — actor '%s' not found"),
			*ContainerActorLabel);
		return Out;
	}

	Out.Reserve(Pts.Num());
	for (const FVector& P : Pts)
	{
		const bool bIsInside = BridgeProceduralImpl::IsPointInsideActor(Container, P);
		if (bIsInside == bInside)
		{
			Out.Add(P);
		}
	}
	return Out;
}

TArray<FVector> UUnrealBridgeProceduralLibrary::FilterPointsByLandscapeLayer(
	const TArray<FVector>& Pts, const FString& LandscapeLabel, FName LayerName, float Threshold)
{
	TArray<FVector> Out;
	if (Pts.Num() == 0)
	{
		return Out;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Target = BridgeProceduralImpl::FindActor(World, LandscapeLabel);
	ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Target);
	if (!Proxy)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: FilterPointsByLandscapeLayer — '%s' not a Landscape actor"),
			*LandscapeLabel);
		return Out;
	}

	ULandscapeInfo* Info = Proxy->GetLandscapeInfo();
	if (!Info)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: FilterPointsByLandscapeLayer — no LandscapeInfo for '%s'"),
			*LandscapeLabel);
		return Out;
	}

	// Collect all landscape components from root + streaming proxies.
	TArray<ULandscapeComponent*> AllComponents;
	Info->ForEachLandscapeProxy([&AllComponents](ALandscapeProxy* P) -> bool
	{
		if (P)
		{
			AllComponents.Append(P->LandscapeComponents);
		}
		return true;
	});

	if (AllComponents.Num() == 0)
	{
		return Out;
	}

	const float SafeThreshold = FMath::Clamp(Threshold, 0.f, 1.f);

	Out.Reserve(Pts.Num());
	for (const FVector& P : Pts)
	{
		// Find the landscape component whose bounds contain this XY. Iterating
		// every component per point is O(P × C); for large landscapes a spatial
		// index would help but isn't worth it at the 100k point cap.
		float Weight = 0.f;
		bool bHit = false;
		for (ULandscapeComponent* C : AllComponents)
		{
			if (!C) continue;
			const FBoxSphereBounds B = C->Bounds;
			const FVector2D MinXY(B.Origin.X - B.BoxExtent.X, B.Origin.Y - B.BoxExtent.Y);
			const FVector2D MaxXY(B.Origin.X + B.BoxExtent.X, B.Origin.Y + B.BoxExtent.Y);
			if (P.X < MinXY.X || P.X > MaxXY.X || P.Y < MinXY.Y || P.Y > MaxXY.Y)
			{
				continue;
			}
			Weight = C->EditorGetPaintLayerWeightByNameAtLocation(P, LayerName);
			bHit = true;
			break;
		}
		if (bHit && Weight >= SafeThreshold)
		{
			Out.Add(P);
		}
	}
	return Out;
}

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

bool UUnrealBridgeProceduralLibrary::RemoveInstancesByIds(
	const FString& ActorName, const TArray<int32>& InstanceIds)
{
	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, ActorName);
	if (!Actor)
	{
		return false;
	}
	UInstancedStaticMeshComponent* ISMC = BridgeProceduralImpl::FindISMComponent(Actor);
	if (!ISMC)
	{
		return false;
	}

	// Filter negatives, sort descending, dedup.
	TArray<int32> Sorted;
	Sorted.Reserve(InstanceIds.Num());
	for (int32 Id : InstanceIds)
	{
		if (Id >= 0)
		{
			Sorted.Add(Id);
		}
	}
	Sorted.Sort([](int32 A, int32 B) { return A > B; });
	for (int32 i = Sorted.Num() - 2; i >= 0; --i)
	{
		if (Sorted[i] == Sorted[i + 1])
		{
			Sorted.RemoveAt(i + 1);
		}
	}

	if (Sorted.Num() == 0)
	{
		return true;
	}

	FScopedTransaction Tr(LOCTEXT("RemoveInstancesByIds", "Remove Procedural Instances By Ids"));
	Actor->Modify();
	ISMC->Modify();
	ISMC->RemoveInstances(Sorted, /*bInstanceArrayAlreadySortedInReverseOrder=*/true);
	return true;
}

bool UUnrealBridgeProceduralLibrary::UpdateInstanceTransformsByIds(
	const FString& ActorName, const TArray<int32>& Ids,
	const TArray<FTransform>& NewXs, bool bWorldSpace)
{
	if (Ids.Num() != NewXs.Num())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: UpdateInstanceTransformsByIds — Ids (%d) and NewXs (%d) length mismatch"),
			Ids.Num(), NewXs.Num());
		return false;
	}

	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, ActorName);
	if (!Actor)
	{
		return false;
	}
	UInstancedStaticMeshComponent* ISMC = BridgeProceduralImpl::FindISMComponent(Actor);
	if (!ISMC)
	{
		return false;
	}

	FScopedTransaction Tr(LOCTEXT("UpdateInstanceTransformsByIds", "Update Procedural Instances By Ids"));
	Actor->Modify();
	ISMC->Modify();

	for (int32 i = 0; i < Ids.Num(); ++i)
	{
		ISMC->UpdateInstanceTransform(Ids[i], NewXs[i],
			bWorldSpace, /*bMarkRenderStateDirty=*/false, /*bTeleport=*/false);
	}
	ISMC->MarkRenderStateDirty();
	return true;
}

bool UUnrealBridgeProceduralLibrary::RebuildProceduralNavigation(const FString& ActorName)
{
	UWorld* World = BridgeProceduralImpl::GetEditorWorld();
	AActor* Actor = BridgeProceduralImpl::FindActor(World, ActorName);
	if (!Actor)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridge: RebuildProceduralNavigation — actor '%s' not found"), *ActorName);
		return false;
	}

	UInstancedStaticMeshComponent* ISMC = BridgeProceduralImpl::FindISMComponent(Actor);
	if (!ISMC)
	{
		return false;
	}

	// HISM-only: synchronously flush the deferred cluster-tree rebuild that
	// AddInstances skipped via bUpdateNavigation=false. Plain ISM doesn't have
	// a cluster tree, so the cast just guards the call.
	if (UHierarchicalInstancedStaticMeshComponent* HISMC =
			Cast<UHierarchicalInstancedStaticMeshComponent>(ISMC))
	{
		HISMC->BuildTreeIfOutdated(/*Async=*/false, /*ForceUpdate=*/true);
	}

	// Re-register with the nav octree — mirrors what AddInstances would have
	// done with bUpdateNavigation=true. Single batch update vs N per-call updates.
	FNavigationSystem::UpdateComponentData(*ISMC);

	return true;
}

#undef LOCTEXT_NAMESPACE
