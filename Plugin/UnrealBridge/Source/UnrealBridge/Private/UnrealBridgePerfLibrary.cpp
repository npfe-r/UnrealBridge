#include "UnrealBridgePerfLibrary.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "LevelEditorViewport.h"
#include "UnrealClient.h"
#include "RenderTimer.h"
#include "MultiGPU.h"
#include "RHIStats.h"
#include "RHIGlobals.h"
#include "DynamicRHI.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMemory.h"
#include "HAL/FileManager.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "WorldPartition/WorldPartition.h"

// GAverageFPS / GAverageMS are defined in UnrealEngine.cpp and have no
// canonical public header declaration — consumers (UnrealEdMisc, etc.) declare
// them inline. Follow that convention.
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

namespace BridgePerfImpl
{
	static constexpr int64 BytesPerMb = 1024LL * 1024LL;

	static int64 BytesToMb(uint64 Bytes)
	{
		return static_cast<int64>(Bytes / BytesPerMb);
	}

	/** Pull the smoothed FStatUnitData from the first active level viewport, if any. */
	static FStatUnitData* GetActiveViewportStatUnit()
	{
		if (!FModuleManager::Get().IsModuleLoaded("LevelEditor"))
		{
			return nullptr;
		}
		FLevelEditorModule& LE = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		TSharedPtr<SLevelViewport> Viewport = LE.GetFirstActiveLevelViewport();
		if (!Viewport.IsValid())
		{
			return nullptr;
		}
		FLevelEditorViewportClient& Client = Viewport->GetLevelViewportClient();
		return Client.GetStatUnitData();
	}

	/** Sum GPU frame time across MAX_NUM_GPUS (FStatUnitData stores per-GPU). */
	static float SumGpuMs(const float (&PerGpuMs)[MAX_NUM_GPUS])
	{
		float Total = 0.f;
		for (int32 i = 0; i < MAX_NUM_GPUS; ++i)
		{
			Total += PerGpuMs[i];
		}
		return Total;
	}

	/** Pick the editor world for breakdown queries — these are introspection
	 *  ops on the level the user has open, never the live PIE world. */
	static UWorld* GetEditorWorldForPerf()
	{
		if (GEditor)
		{
			return GEditor->GetEditorWorldContext().World();
		}
		return nullptr;
	}

	/** Short level name (package short name) for breakdown row keys. */
	static FString GetLevelShortName(const ULevel* Level)
	{
		if (!Level)
		{
			return FString();
		}
		const UPackage* Pkg = Level->GetOutermost();
		if (!Pkg)
		{
			return FString();
		}
		return FPackageName::GetShortName(Pkg->GetName());
	}

	/** Sort breakdown rows by (Count desc, Key asc) and clamp to MaxGroups. */
	static void FinalizeBreakdownRows(
		TArray<FBridgePerfBreakdownRow>& Rows,
		int32 MaxGroups)
	{
		Rows.Sort([](const FBridgePerfBreakdownRow& A, const FBridgePerfBreakdownRow& B)
		{
			if (A.Count != B.Count)
			{
				return A.Count > B.Count;
			}
			if (A.TotalBytes != B.TotalBytes)
			{
				return A.TotalBytes > B.TotalBytes;
			}
			return A.Key < B.Key;
		});
		const int32 Clamp = FMath::Clamp(MaxGroups, 1, 100000);
		if (Rows.Num() > Clamp)
		{
			Rows.SetNum(Clamp);
		}
	}
}

// ─── Frame timing ───────────────────────────────────────────

FBridgeFrameTiming UUnrealBridgePerfLibrary::GetFrameTiming()
{
	FBridgeFrameTiming Out;

	Out.Fps = GAverageFPS;
	Out.FrameMs = (GAverageFPS > 0.f) ? (1000.f / GAverageFPS) : GAverageMS;
	Out.DeltaSeconds = static_cast<float>(FApp::GetDeltaTime());
	Out.FrameNumber = static_cast<int64>(GFrameCounter);

	// Raw per-frame cycle counters — always updated by FViewport::Draw and the
	// renderer, independent of whether `stat unit` is displayed.
	Out.GameThreadMs = FPlatformTime::ToMilliseconds(GGameThreadTime);
	Out.RenderThreadMs = FPlatformTime::ToMilliseconds(GRenderThreadTime);
	{
		// RHIGetGPUFrameCycles is the UE 5.6+ replacement for the deprecated
		// GGPUFrameTime global. Sum per-GPU for MGPU builds.
		uint32 GpuCycles = 0;
		for (uint32 i = 0; i < GNumExplicitGPUsForRendering; ++i)
		{
			GpuCycles += RHIGetGPUFrameCycles(i);
		}
		Out.GpuMs = FPlatformTime::ToMilliseconds(GpuCycles);
	}
	Out.RhiMs = 0.f;
	Out.bSmoothed = false;

	// FStatUnitData is only populated when `stat unit` is actively being drawn
	// on a viewport (FStatUnitData::DrawStat is the sole writer). When the
	// struct has a non-zero FrameTime, the user has stat unit enabled and the
	// smoothed running averages are more stable than our raw snapshot — use
	// them. Otherwise stick with the raw values above.
	if (FStatUnitData* StatUnit = BridgePerfImpl::GetActiveViewportStatUnit())
	{
		if (StatUnit->FrameTime > 0.f)
		{
			Out.bSmoothed = true;
			Out.GameThreadMs = StatUnit->GameThreadTime;
			Out.RenderThreadMs = StatUnit->RenderThreadTime;
			Out.GpuMs = BridgePerfImpl::SumGpuMs(StatUnit->GPUFrameTime);
			Out.RhiMs = StatUnit->RHITTime;
			Out.FrameMs = StatUnit->FrameTime;
		}
	}

	return Out;
}

// ─── Render counters ────────────────────────────────────────

FBridgeRenderCounters UUnrealBridgePerfLibrary::GetRenderCounters()
{
	FBridgeRenderCounters Out;

	int64 TotalDraws = 0;
	int64 TotalPrims = 0;
	for (int32 i = 0; i < MAX_NUM_GPUS; ++i)
	{
		TotalDraws += GNumDrawCallsRHI[i];
		TotalPrims += GNumPrimitivesDrawnRHI[i];
	}

	Out.DrawCalls = static_cast<int32>(FMath::Min<int64>(TotalDraws, MAX_int32));
	Out.PrimitivesDrawn = static_cast<int32>(FMath::Min<int64>(TotalPrims, MAX_int32));
	Out.NumGpus = static_cast<int32>(GNumExplicitGPUsForRendering);
	return Out;
}

// ─── Memory ─────────────────────────────────────────────────

FBridgeMemoryStats UUnrealBridgePerfLibrary::GetMemoryStats()
{
	FBridgeMemoryStats Out;

	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	const FPlatformMemoryConstants& Constants = FPlatformMemory::GetConstants();

	Out.UsedPhysicalMb = BridgePerfImpl::BytesToMb(Stats.UsedPhysical);
	Out.UsedVirtualMb = BridgePerfImpl::BytesToMb(Stats.UsedVirtual);
	Out.PeakUsedPhysicalMb = BridgePerfImpl::BytesToMb(Stats.PeakUsedPhysical);
	Out.PeakUsedVirtualMb = BridgePerfImpl::BytesToMb(Stats.PeakUsedVirtual);
	Out.AvailablePhysicalMb = BridgePerfImpl::BytesToMb(Stats.AvailablePhysical);
	Out.AvailableVirtualMb = BridgePerfImpl::BytesToMb(Stats.AvailableVirtual);
	Out.TotalPhysicalMb = BridgePerfImpl::BytesToMb(Constants.TotalPhysical);

	return Out;
}

// ─── UObject histogram ──────────────────────────────────────

FBridgeUObjectStats UUnrealBridgePerfLibrary::GetUObjectStats(int32 TopN)
{
	FBridgeUObjectStats Out;

	const int32 ClampedTopN = FMath::Clamp(TopN, 1, 200);

	TMap<FName, int32> Counts;
	Counts.Reserve(4096);

	int32 TotalObjects = 0;
	for (TObjectIterator<UObject> It; It; ++It)
	{
		UObject* Obj = *It;
		if (!Obj)
		{
			continue;
		}
		UClass* Cls = Obj->GetClass();
		if (!Cls)
		{
			continue;
		}
		++Counts.FindOrAdd(Cls->GetFName());
		++TotalObjects;
	}

	Out.TotalObjects = TotalObjects;
	Out.UniqueClasses = Counts.Num();

	TArray<TPair<FName, int32>> Sorted;
	Sorted.Reserve(Counts.Num());
	for (const TPair<FName, int32>& Entry : Counts)
	{
		Sorted.Emplace(Entry);
	}
	Sorted.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B)
	{
		return A.Value > B.Value;
	});

	const int32 Take = FMath::Min(ClampedTopN, Sorted.Num());
	Out.TopClasses.Reserve(Take);
	for (int32 i = 0; i < Take; ++i)
	{
		FBridgeUObjectClassCount Row;
		Row.ClassName = Sorted[i].Key.ToString();
		Row.Count = Sorted[i].Value;
		Out.TopClasses.Add(MoveTemp(Row));
	}

	return Out;
}

// ─── Aggregate snapshot ─────────────────────────────────────

FBridgePerfSnapshot UUnrealBridgePerfLibrary::GetPerfSnapshot(bool bIncludeUObjectStats, int32 UObjectTopN)
{
	FBridgePerfSnapshot Out;

	Out.Timing = GetFrameTiming();
	Out.Render = GetRenderCounters();
	Out.Memory = GetMemoryStats();
	if (bIncludeUObjectStats)
	{
		Out.UObjects = GetUObjectStats(UObjectTopN);
	}
	Out.CaptureTimeUtc = FDateTime::UtcNow().ToIso8601();
	Out.EngineVersion = FEngineVersion::Current().ToString();
	Out.bWasInPie = (GEditor && GEditor->PlayWorld != nullptr);

	return Out;
}

// ─── World actor breakdown (M1-3) ───────────────────────────

namespace BridgePerfImpl
{
	/** Per-key accumulator: count + first 3 sample paths. */
	struct FActorBreakdownAcc
	{
		int32 Count = 0;
		TArray<FString> Samples;
	};

	/** Build a composite map key as TPair<LevelName, ClassName>. TPair has
	 *  built-in GetTypeHash via TPair's ADL hook, so no custom hash needed.
	 *  Either component is NAME_None when the corresponding group_by mode
	 *  doesn't include it. */
	using FActorBreakdownKey = TPair<FName, FName>;
}

TArray<FBridgePerfBreakdownRow> UUnrealBridgePerfLibrary::GetWorldActorBreakdown(
	const FString& LevelFilter,
	const FString& GroupBy,
	int32 MaxGroups)
{
	TArray<FBridgePerfBreakdownRow> Out;

	UWorld* World = BridgePerfImpl::GetEditorWorldForPerf();
	if (!World)
	{
		return Out;
	}

	// Validate group_by; fall back to "class" silently to avoid empty results.
	const bool bGroupByLevel = GroupBy.Equals(TEXT("level"), ESearchCase::IgnoreCase);
	const bool bGroupByLevelClass = GroupBy.Equals(TEXT("level_class"), ESearchCase::IgnoreCase);
	const bool bGroupByClass = !bGroupByLevel && !bGroupByLevelClass;

	// World Partition projects: GetLevels() only returns currently-loaded
	// actors. Listing unloaded actor descs requires the WP ActorDescContainer
	// API which has churned across 5.3-5.7 (ActorDescContainer →
	// ActorDescContainerInstance → ActorDescContainerCollection). For now
	// return whatever GetLevels() gives and warn that WP has been detected;
	// a separate UFUNCTION can list unloaded descs in a follow-up milestone.
	// TODO(M1+): partition-aware enumeration via UWorldPartition::ForEachActorDescInstance
	const bool bIsWorldPartition = (World->GetWorldPartition() != nullptr);
	if (bIsWorldPartition)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UnrealBridgePerf: GetWorldActorBreakdown — World Partition detected; ")
			TEXT("only loaded actors counted (unloaded descs not yet supported)"));
	}

	// Walk levels, build (level, class) → count + samples map.
	TMap<BridgePerfImpl::FActorBreakdownKey, BridgePerfImpl::FActorBreakdownAcc> Acc;
	Acc.Reserve(256);

	const FString TrimmedFilter = LevelFilter.TrimStartAndEnd();

	for (ULevel* Level : World->GetLevels())
	{
		if (!Level)
		{
			continue;
		}
		const FString LevelShort = BridgePerfImpl::GetLevelShortName(Level);
		if (!TrimmedFilter.IsEmpty() && !LevelShort.Contains(TrimmedFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FName LevelFName(*LevelShort);

		for (AActor* Actor : Level->Actors)
		{
			if (!Actor || !IsValid(Actor))
			{
				continue;
			}
			UClass* Cls = Actor->GetClass();
			if (!Cls)
			{
				continue;
			}

			// TPair<LevelName, ClassName>. NAME_None for the dimension we
			// aren't grouping on.
			BridgePerfImpl::FActorBreakdownKey MapKey;
			if (bGroupByLevel)
			{
				MapKey = BridgePerfImpl::FActorBreakdownKey(LevelFName, NAME_None);
			}
			else if (bGroupByLevelClass)
			{
				MapKey = BridgePerfImpl::FActorBreakdownKey(LevelFName, Cls->GetFName());
			}
			else // bGroupByClass
			{
				MapKey = BridgePerfImpl::FActorBreakdownKey(NAME_None, Cls->GetFName());
			}

			BridgePerfImpl::FActorBreakdownAcc& Bucket = Acc.FindOrAdd(MapKey);
			++Bucket.Count;
			if (Bucket.Samples.Num() < 3)
			{
				Bucket.Samples.Add(Actor->GetPathName());
			}
		}
	}

	// Materialise into output rows. TPair: .Key=LevelName, .Value=ClassName.
	Out.Reserve(Acc.Num());
	for (const TPair<BridgePerfImpl::FActorBreakdownKey, BridgePerfImpl::FActorBreakdownAcc>& Entry : Acc)
	{
		const FName LevelKeyComp = Entry.Key.Key;
		const FName ClassKeyComp = Entry.Key.Value;

		FBridgePerfBreakdownRow Row;
		if (bGroupByLevel)
		{
			Row.Key = LevelKeyComp.ToString();
		}
		else if (bGroupByLevelClass)
		{
			Row.Key = ClassKeyComp.ToString();
			Row.LevelName = LevelKeyComp.ToString();
		}
		else // bGroupByClass
		{
			Row.Key = ClassKeyComp.ToString();
		}
		Row.Count = Entry.Value.Count;
		Row.TotalBytes = 0; // actors have no on-disk size in this view
		Row.SamplePaths = Entry.Value.Samples;
		Out.Add(MoveTemp(Row));
	}

	BridgePerfImpl::FinalizeBreakdownRows(Out, MaxGroups);
	return Out;
}
