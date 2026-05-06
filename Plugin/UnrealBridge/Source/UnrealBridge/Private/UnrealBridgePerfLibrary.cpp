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
#include "Engine/Texture.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Sound/SoundWave.h"
#include "StaticMeshResources.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"

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

	/** Sort breakdown rows by (TotalBytes desc, Count desc, Key asc). Used for
	 *  asset memory breakdowns where bytes are the primary perf signal. */
	static void FinalizeBreakdownRowsByBytes(
		TArray<FBridgePerfBreakdownRow>& Rows,
		int32 MaxGroups)
	{
		Rows.Sort([](const FBridgePerfBreakdownRow& A, const FBridgePerfBreakdownRow& B)
		{
			if (A.TotalBytes != B.TotalBytes)
			{
				return A.TotalBytes > B.TotalBytes;
			}
			if (A.Count != B.Count)
			{
				return A.Count > B.Count;
			}
			return A.Key < B.Key;
		});
		const int32 Clamp = FMath::Clamp(MaxGroups, 1, 100000);
		if (Rows.Num() > Clamp)
		{
			Rows.SetNum(Clamp);
		}
	}

	/** On-disk size for a package, in bytes. Works on every UE version because
	 *  IAssetRegistry::GetAssetSizeOnDisk doesn't exist on stock 5.3-5.7 —
	 *  we always go through DoesPackageExist + FileSize. Returns 0 for
	 *  never-saved or missing packages. */
	static int64 GetPackageDiskSize(FName PackageName)
	{
		if (PackageName.IsNone())
		{
			return 0;
		}
		const FString PackageStr = PackageName.ToString();
		FString Filename;
		if (!FPackageName::DoesPackageExist(PackageStr, &Filename))
		{
			return 0;
		}
		const int64 Size = IFileManager::Get().FileSize(*Filename);
		return Size > 0 ? Size : 0;
	}

	/** Extract the leading content folder from a package path, e.g.
	 *  "/Game/Characters/Hero/T_Skin" → "/Game/Characters". One level deep —
	 *  callers wanting deeper bucketing can post-process. */
	static FString GetTopLevelFolder(const FString& PackagePath)
	{
		// PackagePath is the directory portion (e.g. "/Game/Characters/Hero").
		// Strip a leading slash, take up to the first two components, re-prefix.
		FString Trimmed = PackagePath;
		if (Trimmed.StartsWith(TEXT("/")))
		{
			Trimmed.RemoveAt(0);
		}
		int32 SecondSlash = INDEX_NONE;
		int32 FirstSlash = INDEX_NONE;
		if (Trimmed.FindChar(TEXT('/'), FirstSlash))
		{
			Trimmed.FindChar(TEXT('/'), SecondSlash);
			if (FirstSlash != INDEX_NONE)
			{
				int32 Pos = FirstSlash + 1;
				int32 NextSlash = INDEX_NONE;
				if (Trimmed.RightChop(Pos).FindChar(TEXT('/'), NextSlash))
				{
					return TEXT("/") + Trimmed.Left(Pos + NextSlash);
				}
			}
		}
		// No second slash — the whole thing is the top folder.
		return TEXT("/") + Trimmed;
	}

	/** Add (or update) one breakdown bucket. Maintains Count, TotalBytes, and
	 *  up to 3 sample paths in insertion order. */
	static void AccumulateBucket(
		TMap<FString, FBridgePerfBreakdownRow>& Buckets,
		const FString& Key,
		int64 BytesToAdd,
		const FString& SamplePath)
	{
		FBridgePerfBreakdownRow& Bucket = Buckets.FindOrAdd(Key);
		if (Bucket.Key.IsEmpty())
		{
			Bucket.Key = Key;
		}
		Bucket.Count += 1;
		Bucket.TotalBytes += BytesToAdd;
		if (Bucket.SamplePaths.Num() < 3)
		{
			Bucket.SamplePaths.Add(SamplePath);
		}
	}

	/** Map a TextureCompressionSettings enum value (TC_*) to a coarse sampler
	 *  type bucket name, mirroring UE's GetSamplerTypeForCompressionSettings
	 *  conventions but without needing the texture loaded. Used by group_by
	 *  ="sampler_type" in disk mode. */
	static FString CompressionSettingsToSamplerBucket(const FString& TcEnumString)
	{
		// Strings come back as the enum identifier (e.g. "TC_Normalmap").
		if (TcEnumString.Equals(TEXT("TC_Normalmap"), ESearchCase::IgnoreCase))
		{
			return TEXT("Normal");
		}
		if (TcEnumString.Equals(TEXT("TC_Masks"), ESearchCase::IgnoreCase) ||
			TcEnumString.Equals(TEXT("TC_Alpha"), ESearchCase::IgnoreCase) ||
			TcEnumString.Equals(TEXT("TC_Grayscale"), ESearchCase::IgnoreCase))
		{
			return TEXT("Masks");
		}
		if (TcEnumString.Contains(TEXT("HDR")) ||
			TcEnumString.Contains(TEXT("HighDynamicRange")) ||
			TcEnumString.Equals(TEXT("TC_HalfFloat"), ESearchCase::IgnoreCase) ||
			TcEnumString.Equals(TEXT("TC_SingleFloat"), ESearchCase::IgnoreCase))
		{
			return TEXT("LinearColor");
		}
		if (TcEnumString.Equals(TEXT("TC_Displacementmap"), ESearchCase::IgnoreCase) ||
			TcEnumString.Equals(TEXT("TC_VectorDisplacementmap"), ESearchCase::IgnoreCase))
		{
			return TEXT("Displacement");
		}
		if (TcEnumString.Equals(TEXT("TC_DistanceFieldFont"), ESearchCase::IgnoreCase))
		{
			return TEXT("DistanceFieldFont");
		}
		// TC_Default, TC_BC7, TC_LQ, etc. — color sampler.
		return TEXT("Color");
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

// ─── Texture memory breakdown (M1-1) ────────────────────────

namespace BridgePerfImpl
{
	/** Texture group_by mode. Translated once up front to avoid per-asset string compares. */
	enum class ETextureGroupBy : uint8
	{
		Folder,
		LodGroup,
		CompressionFormat,
		SamplerType,
	};

	static bool ParseTextureGroupBy(const FString& In, ETextureGroupBy& Out)
	{
		if (In.Equals(TEXT("folder"), ESearchCase::IgnoreCase)) { Out = ETextureGroupBy::Folder; return true; }
		if (In.Equals(TEXT("lod_group"), ESearchCase::IgnoreCase)) { Out = ETextureGroupBy::LodGroup; return true; }
		if (In.Equals(TEXT("compression_format"), ESearchCase::IgnoreCase)) { Out = ETextureGroupBy::CompressionFormat; return true; }
		if (In.Equals(TEXT("sampler_type"), ESearchCase::IgnoreCase)) { Out = ETextureGroupBy::SamplerType; return true; }
		return false;
	}

	static FString GetTextureGroupKeyFromAssetData(
		const FAssetData& Data,
		ETextureGroupBy Mode)
	{
		switch (Mode)
		{
		case ETextureGroupBy::Folder:
		{
			return GetTopLevelFolder(Data.PackagePath.ToString());
		}
		case ETextureGroupBy::LodGroup:
		{
			FString Tag;
			if (Data.GetTagValue(TEXT("LODGroup"), Tag) && !Tag.IsEmpty())
			{
				return Tag;
			}
			return TEXT("(unspecified)");
		}
		case ETextureGroupBy::CompressionFormat:
		{
			FString Tag;
			if (Data.GetTagValue(TEXT("CompressionSettings"), Tag) && !Tag.IsEmpty())
			{
				return Tag;
			}
			return TEXT("(unspecified)");
		}
		case ETextureGroupBy::SamplerType:
		{
			FString Tag;
			if (Data.GetTagValue(TEXT("CompressionSettings"), Tag) && !Tag.IsEmpty())
			{
				return CompressionSettingsToSamplerBucket(Tag);
			}
			return TEXT("Color"); // TC_Default fallback
		}
		}
		return TEXT("(unknown)");
	}

	static FString GetTextureGroupKeyFromObject(
		const UTexture* Tex,
		ETextureGroupBy Mode)
	{
		if (!Tex)
		{
			return TEXT("(null)");
		}
		switch (Mode)
		{
		case ETextureGroupBy::Folder:
		{
			const UPackage* Pkg = Tex->GetOutermost();
			if (!Pkg) return TEXT("(transient)");
			// Outer package name is /Game/Foo/Bar/PackageName — strip filename.
			const FString PackageName = Pkg->GetName();
			int32 LastSlash = INDEX_NONE;
			if (PackageName.FindLastChar(TEXT('/'), LastSlash))
			{
				return GetTopLevelFolder(PackageName.Left(LastSlash));
			}
			return PackageName;
		}
		case ETextureGroupBy::LodGroup:
		{
			const UEnum* Enum = StaticEnum<TextureGroup>();
			if (Enum)
			{
				return Enum->GetNameStringByValue(static_cast<int64>(Tex->LODGroup));
			}
			return FString::FromInt(static_cast<int32>(Tex->LODGroup));
		}
		case ETextureGroupBy::CompressionFormat:
		{
			const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
			if (Enum)
			{
				return Enum->GetNameStringByValue(static_cast<int64>(Tex->CompressionSettings));
			}
			return FString::FromInt(static_cast<int32>(Tex->CompressionSettings));
		}
		case ETextureGroupBy::SamplerType:
		{
			const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
			if (Enum)
			{
				const FString TcStr = Enum->GetNameStringByValue(static_cast<int64>(Tex->CompressionSettings));
				return CompressionSettingsToSamplerBucket(TcStr);
			}
			return TEXT("Color");
		}
		}
		return TEXT("(unknown)");
	}
}

TArray<FBridgePerfBreakdownRow> UUnrealBridgePerfLibrary::GetTextureMemoryBreakdown(
	const FString& GroupBy,
	const FString& Mode,
	int32 MaxGroups)
{
	TArray<FBridgePerfBreakdownRow> Out;

	BridgePerfImpl::ETextureGroupBy ModeEnum = BridgePerfImpl::ETextureGroupBy::Folder;
	if (!BridgePerfImpl::ParseTextureGroupBy(GroupBy, ModeEnum))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridgePerf: GetTextureMemoryBreakdown bad group_by '%s' — expected ")
			TEXT("folder | lod_group | compression_format | sampler_type"),
			*GroupBy);
		return Out;
	}

	const bool bRuntimeMode = Mode.Equals(TEXT("runtime"), ESearchCase::IgnoreCase);
	const bool bDiskMode = Mode.IsEmpty() || Mode.Equals(TEXT("disk"), ESearchCase::IgnoreCase);
	if (!bRuntimeMode && !bDiskMode)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridgePerf: GetTextureMemoryBreakdown bad mode '%s' — expected disk | runtime"),
			*Mode);
		return Out;
	}

	TMap<FString, FBridgePerfBreakdownRow> Buckets;
	Buckets.Reserve(64);

	if (bRuntimeMode)
	{
		// Iterate every loaded UTexture (covers Texture2D/Cube/Volume/etc.).
		// Only objects already in memory are counted — this is the contract.
		for (TObjectIterator<UTexture> It; It; ++It)
		{
			UTexture* Tex = *It;
			if (!Tex || !IsValid(Tex))
			{
				continue;
			}
			const int64 Bytes = static_cast<int64>(
				Tex->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal));
			const FString Key = BridgePerfImpl::GetTextureGroupKeyFromObject(Tex, ModeEnum);
			BridgePerfImpl::AccumulateBucket(Buckets, Key, Bytes, Tex->GetPathName());
		}
	}
	else
	{
		// Disk mode: walk AssetRegistry, no LoadObject. Skip never-saved
		// assets (TagsAndValues empty + no on-disk file → disk size 0 — they
		// legitimately don't contribute to project disk footprint).
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(UTexture::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);

		for (const FAssetData& Data : Assets)
		{
			if (!Data.IsValid())
			{
				continue;
			}
			const int64 Bytes = BridgePerfImpl::GetPackageDiskSize(Data.PackageName);
			if (Bytes <= 0)
			{
				// Never-saved or missing on disk. Skip (matches doc contract).
				continue;
			}
			const FString Key = BridgePerfImpl::GetTextureGroupKeyFromAssetData(Data, ModeEnum);
			BridgePerfImpl::AccumulateBucket(Buckets, Key, Bytes, Data.GetSoftObjectPath().ToString());
		}
	}

	Out.Reserve(Buckets.Num());
	for (const TPair<FString, FBridgePerfBreakdownRow>& Pair : Buckets)
	{
		Out.Add(Pair.Value);
	}
	BridgePerfImpl::FinalizeBreakdownRowsByBytes(Out, MaxGroups);
	return Out;
}

// ─── Mesh memory breakdown (M1-2) ───────────────────────────

namespace BridgePerfImpl
{
	enum class EMeshGroupBy : uint8
	{
		Folder,
		LodCount,
		VertexCountBucket,
	};

	static bool ParseMeshGroupBy(const FString& In, EMeshGroupBy& Out)
	{
		if (In.Equals(TEXT("folder"), ESearchCase::IgnoreCase)) { Out = EMeshGroupBy::Folder; return true; }
		if (In.Equals(TEXT("lod_count"), ESearchCase::IgnoreCase)) { Out = EMeshGroupBy::LodCount; return true; }
		if (In.Equals(TEXT("vertex_count_bucket"), ESearchCase::IgnoreCase)) { Out = EMeshGroupBy::VertexCountBucket; return true; }
		return false;
	}

	/** Map a vertex count to a coarse log-scale bucket. */
	static FString VertexCountToBucket(int64 N)
	{
		if (N < 0) N = 0;
		if (N < 1000)        return TEXT("<1k");
		if (N < 10000)       return TEXT("1k-10k");
		if (N < 100000)      return TEXT("10k-100k");
		if (N < 1000000)     return TEXT("100k-1M");
		return TEXT(">=1M");
	}

	/** Read a numeric tag from FAssetData; returns Default when missing. */
	static int64 GetTagInt(const FAssetData& Data, FName Tag, int64 Default)
	{
		FString Value;
		if (Data.GetTagValue(Tag, Value) && !Value.IsEmpty())
		{
			return FCString::Atoi64(*Value);
		}
		return Default;
	}

	static FString GetMeshGroupKeyFromAssetData(const FAssetData& Data, EMeshGroupBy Mode)
	{
		switch (Mode)
		{
		case EMeshGroupBy::Folder:
			return GetTopLevelFolder(Data.PackagePath.ToString());
		case EMeshGroupBy::LodCount:
		{
			const int64 N = GetTagInt(Data, TEXT("LODs"), -1);
			if (N < 0) return TEXT("(unknown)");
			return FString::Printf(TEXT("%d LOD%s"), static_cast<int32>(N), (N == 1 ? TEXT("") : TEXT("s")));
		}
		case EMeshGroupBy::VertexCountBucket:
		{
			const int64 N = GetTagInt(Data, TEXT("Vertices"), -1);
			if (N < 0) return TEXT("(unknown)");
			return VertexCountToBucket(N);
		}
		}
		return TEXT("(unknown)");
	}
}

TArray<FBridgePerfBreakdownRow> UUnrealBridgePerfLibrary::GetMeshMemoryBreakdown(
	const FString& GroupBy,
	const FString& MeshType,
	const FString& Mode,
	int32 MaxGroups)
{
	TArray<FBridgePerfBreakdownRow> Out;

	BridgePerfImpl::EMeshGroupBy ModeEnum = BridgePerfImpl::EMeshGroupBy::Folder;
	if (!BridgePerfImpl::ParseMeshGroupBy(GroupBy, ModeEnum))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridgePerf: GetMeshMemoryBreakdown bad group_by '%s' — expected ")
			TEXT("folder | lod_count | vertex_count_bucket"),
			*GroupBy);
		return Out;
	}

	const bool bWantStatic = MeshType.IsEmpty()
		|| MeshType.Equals(TEXT("all"), ESearchCase::IgnoreCase)
		|| MeshType.Equals(TEXT("static"), ESearchCase::IgnoreCase);
	const bool bWantSkeletal = MeshType.IsEmpty()
		|| MeshType.Equals(TEXT("all"), ESearchCase::IgnoreCase)
		|| MeshType.Equals(TEXT("skeletal"), ESearchCase::IgnoreCase);
	if (!bWantStatic && !bWantSkeletal)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridgePerf: GetMeshMemoryBreakdown bad mesh_type '%s' — expected static | skeletal | all"),
			*MeshType);
		return Out;
	}

	const bool bRuntimeMode = Mode.Equals(TEXT("runtime"), ESearchCase::IgnoreCase);
	const bool bDiskMode = Mode.IsEmpty() || Mode.Equals(TEXT("disk"), ESearchCase::IgnoreCase);
	if (!bRuntimeMode && !bDiskMode)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridgePerf: GetMeshMemoryBreakdown bad mode '%s' — expected disk | runtime"),
			*Mode);
		return Out;
	}

	TMap<FString, FBridgePerfBreakdownRow> Buckets;
	Buckets.Reserve(64);

	if (bRuntimeMode)
	{
		if (bWantStatic)
		{
			for (TObjectIterator<UStaticMesh> It; It; ++It)
			{
				UStaticMesh* SM = *It;
				if (!SM || !IsValid(SM)) continue;
				const int64 Bytes = static_cast<int64>(SM->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal));
				FString Key;
				switch (ModeEnum)
				{
				case BridgePerfImpl::EMeshGroupBy::Folder:
				{
					if (UPackage* Pkg = SM->GetOutermost())
					{
						const FString PackageName = Pkg->GetName();
						int32 LastSlash = INDEX_NONE;
						if (PackageName.FindLastChar(TEXT('/'), LastSlash))
						{
							Key = BridgePerfImpl::GetTopLevelFolder(PackageName.Left(LastSlash));
						}
						else
						{
							Key = PackageName;
						}
					}
					else
					{
						Key = TEXT("(transient)");
					}
					break;
				}
				case BridgePerfImpl::EMeshGroupBy::LodCount:
				{
					const int32 NumLODs = SM->GetNumLODs();
					Key = FString::Printf(TEXT("%d LOD%s"), NumLODs, (NumLODs == 1 ? TEXT("") : TEXT("s")));
					break;
				}
				case BridgePerfImpl::EMeshGroupBy::VertexCountBucket:
				{
					int64 TotalVerts = 0;
					if (FStaticMeshRenderData* RD = SM->GetRenderData())
					{
						for (const FStaticMeshLODResources& LOD : RD->LODResources)
						{
							TotalVerts += LOD.GetNumVertices();
						}
					}
					Key = BridgePerfImpl::VertexCountToBucket(TotalVerts);
					break;
				}
				}
				BridgePerfImpl::AccumulateBucket(Buckets, Key, Bytes, SM->GetPathName());
			}
		}
		if (bWantSkeletal)
		{
			for (TObjectIterator<USkeletalMesh> It; It; ++It)
			{
				USkeletalMesh* SK = *It;
				if (!SK || !IsValid(SK)) continue;
				const int64 Bytes = static_cast<int64>(SK->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal));
				FString Key;
				switch (ModeEnum)
				{
				case BridgePerfImpl::EMeshGroupBy::Folder:
				{
					if (UPackage* Pkg = SK->GetOutermost())
					{
						const FString PackageName = Pkg->GetName();
						int32 LastSlash = INDEX_NONE;
						if (PackageName.FindLastChar(TEXT('/'), LastSlash))
						{
							Key = BridgePerfImpl::GetTopLevelFolder(PackageName.Left(LastSlash));
						}
						else
						{
							Key = PackageName;
						}
					}
					else
					{
						Key = TEXT("(transient)");
					}
					break;
				}
				case BridgePerfImpl::EMeshGroupBy::LodCount:
				{
					int32 NumLODs = 0;
					if (FSkeletalMeshRenderData* RD = SK->GetResourceForRendering())
					{
						NumLODs = RD->LODRenderData.Num();
					}
					Key = FString::Printf(TEXT("%d LOD%s"), NumLODs, (NumLODs == 1 ? TEXT("") : TEXT("s")));
					break;
				}
				case BridgePerfImpl::EMeshGroupBy::VertexCountBucket:
				{
					int64 TotalVerts = 0;
					if (FSkeletalMeshRenderData* RD = SK->GetResourceForRendering())
					{
						for (const FSkeletalMeshLODRenderData& LOD : RD->LODRenderData)
						{
							TotalVerts += LOD.GetNumVertices();
						}
					}
					Key = BridgePerfImpl::VertexCountToBucket(TotalVerts);
					break;
				}
				}
				BridgePerfImpl::AccumulateBucket(Buckets, Key, Bytes, SK->GetPathName());
			}
		}
	}
	else
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		auto WalkAssets = [&Buckets, ModeEnum, &AR](UClass* Cls)
		{
			TArray<FAssetData> Assets;
			AR.GetAssetsByClass(Cls->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
			for (const FAssetData& Data : Assets)
			{
				if (!Data.IsValid()) continue;
				const int64 Bytes = BridgePerfImpl::GetPackageDiskSize(Data.PackageName);
				if (Bytes <= 0) continue;
				const FString Key = BridgePerfImpl::GetMeshGroupKeyFromAssetData(Data, ModeEnum);
				BridgePerfImpl::AccumulateBucket(Buckets, Key, Bytes, Data.GetSoftObjectPath().ToString());
			}
		};
		if (bWantStatic) WalkAssets(UStaticMesh::StaticClass());
		if (bWantSkeletal) WalkAssets(USkeletalMesh::StaticClass());
	}

	Out.Reserve(Buckets.Num());
	for (const TPair<FString, FBridgePerfBreakdownRow>& Pair : Buckets)
	{
		Out.Add(Pair.Value);
	}
	BridgePerfImpl::FinalizeBreakdownRowsByBytes(Out, MaxGroups);
	return Out;
}

// ─── UObject memory breakdown (M1-4) ────────────────────────

TArray<FBridgePerfBreakdownRow> UUnrealBridgePerfLibrary::GetUObjectMemoryBreakdown(int32 TopN)
{
	TArray<FBridgePerfBreakdownRow> Out;
	const int32 ClampedTopN = FMath::Clamp(TopN, 1, 200);

	// Walk every live UObject once: bump class counter + accumulate exclusive
	// bytes + remember up to 3 sample paths per class.
	TMap<FName, FBridgePerfBreakdownRow> ByClass;
	ByClass.Reserve(4096);

	for (TObjectIterator<UObject> It; It; ++It)
	{
		UObject* Obj = *It;
		if (!Obj) continue;
		UClass* Cls = Obj->GetClass();
		if (!Cls) continue;

		FBridgePerfBreakdownRow& Bucket = ByClass.FindOrAdd(Cls->GetFName());
		if (Bucket.Key.IsEmpty())
		{
			Bucket.Key = Cls->GetFName().ToString();
		}
		Bucket.Count += 1;
		// EResourceSizeMode::Exclusive: only objects "owned" by this UObject;
		// avoids triple-counting referenced material/texture/mesh bytes that
		// would inflate aggregate by orders of magnitude.
		Bucket.TotalBytes += static_cast<int64>(Obj->GetResourceSizeBytes(EResourceSizeMode::Exclusive));
		if (Bucket.SamplePaths.Num() < 3)
		{
			Bucket.SamplePaths.Add(Obj->GetPathName());
		}
	}

	Out.Reserve(ByClass.Num());
	for (const TPair<FName, FBridgePerfBreakdownRow>& Pair : ByClass)
	{
		Out.Add(Pair.Value);
	}
	BridgePerfImpl::FinalizeBreakdownRowsByBytes(Out, ClampedTopN);
	return Out;
}

// ─── Audio memory breakdown (M1-5) ──────────────────────────

namespace BridgePerfImpl
{
	enum class EAudioGroupBy : uint8
	{
		CompressionFormat,
		Folder,
		SampleRateBucket,
		ChannelCount,
	};

	static bool ParseAudioGroupBy(const FString& In, EAudioGroupBy& Out)
	{
		if (In.IsEmpty() || In.Equals(TEXT("compression_format"), ESearchCase::IgnoreCase))
		{
			Out = EAudioGroupBy::CompressionFormat;
			return true;
		}
		if (In.Equals(TEXT("folder"), ESearchCase::IgnoreCase)) { Out = EAudioGroupBy::Folder; return true; }
		if (In.Equals(TEXT("sample_rate_bucket"), ESearchCase::IgnoreCase)) { Out = EAudioGroupBy::SampleRateBucket; return true; }
		if (In.Equals(TEXT("channel_count"), ESearchCase::IgnoreCase)) { Out = EAudioGroupBy::ChannelCount; return true; }
		return false;
	}

	static FString SampleRateToBucket(int64 Hz)
	{
		if (Hz <= 0)        return TEXT("(unknown)");
		if (Hz < 8000)      return TEXT("<8k");
		if (Hz < 16000)     return TEXT("8k-16k");
		if (Hz < 22050)     return TEXT("16k-22k");
		if (Hz < 44100)     return TEXT("22k-44k");
		if (Hz < 48000)     return TEXT("44k-48k");
		if (Hz < 96000)     return TEXT("48k-96k");
		return TEXT(">=96k");
	}

	static FString ChannelCountToBucket(int64 N)
	{
		if (N <= 0) return TEXT("(unknown)");
		if (N == 1) return TEXT("Mono");
		if (N == 2) return TEXT("Stereo");
		if (N == 6) return TEXT("5.1");
		if (N == 8) return TEXT("7.1");
		return TEXT("Other");
	}

	static FString GetAudioGroupKeyFromAssetData(const FAssetData& Data, EAudioGroupBy Mode)
	{
		switch (Mode)
		{
		case EAudioGroupBy::Folder:
			return GetTopLevelFolder(Data.PackagePath.ToString());
		case EAudioGroupBy::CompressionFormat:
		{
			FString Tag;
			// SoundAssetCompressionType is the modern tag (5.0+); covers PCM /
			// ADPCM / BinkAudio / Opus / etc.
			if (Data.GetTagValue(TEXT("SoundAssetCompressionType"), Tag) && !Tag.IsEmpty())
			{
				return Tag;
			}
			return TEXT("(unspecified)");
		}
		case EAudioGroupBy::SampleRateBucket:
		{
			return SampleRateToBucket(GetTagInt(Data, TEXT("SampleRate"), -1));
		}
		case EAudioGroupBy::ChannelCount:
		{
			return ChannelCountToBucket(GetTagInt(Data, TEXT("NumChannels"), -1));
		}
		}
		return TEXT("(unknown)");
	}

	static FString GetAudioGroupKeyFromObject(const USoundWave* SW, EAudioGroupBy Mode)
	{
		if (!SW) return TEXT("(null)");
		switch (Mode)
		{
		case EAudioGroupBy::Folder:
		{
			if (UPackage* Pkg = SW->GetOutermost())
			{
				const FString PackageName = Pkg->GetName();
				int32 LastSlash = INDEX_NONE;
				if (PackageName.FindLastChar(TEXT('/'), LastSlash))
				{
					return GetTopLevelFolder(PackageName.Left(LastSlash));
				}
				return PackageName;
			}
			return TEXT("(transient)");
		}
		case EAudioGroupBy::CompressionFormat:
		{
			const ESoundAssetCompressionType Type = SW->GetSoundAssetCompressionType();
			const UEnum* Enum = StaticEnum<ESoundAssetCompressionType>();
			if (Enum)
			{
				return Enum->GetNameStringByValue(static_cast<int64>(Type));
			}
			return FString::FromInt(static_cast<int32>(Type));
		}
		case EAudioGroupBy::SampleRateBucket:
			return SampleRateToBucket(static_cast<int64>(SW->GetSampleRateForCurrentPlatform()));
		case EAudioGroupBy::ChannelCount:
			// USoundWave::NumChannels is a public int32 UPROPERTY across 5.3-5.7.
			// (5.3 doesn't have a public USoundWave::GetNumChannels() — that
			// method lives on FSoundWaveData/FSoundWaveProxy proxy types only.)
			return ChannelCountToBucket(static_cast<int64>(SW->NumChannels));
		}
		return TEXT("(unknown)");
	}
}

TArray<FBridgePerfBreakdownRow> UUnrealBridgePerfLibrary::GetAudioMemoryBreakdown(
	const FString& GroupBy,
	const FString& Mode,
	int32 MaxGroups)
{
	TArray<FBridgePerfBreakdownRow> Out;

	BridgePerfImpl::EAudioGroupBy ModeEnum = BridgePerfImpl::EAudioGroupBy::CompressionFormat;
	if (!BridgePerfImpl::ParseAudioGroupBy(GroupBy, ModeEnum))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridgePerf: GetAudioMemoryBreakdown bad group_by '%s' — expected ")
			TEXT("compression_format | folder | sample_rate_bucket | channel_count"),
			*GroupBy);
		return Out;
	}

	const bool bRuntimeMode = Mode.Equals(TEXT("runtime"), ESearchCase::IgnoreCase);
	const bool bDiskMode = Mode.IsEmpty() || Mode.Equals(TEXT("disk"), ESearchCase::IgnoreCase);
	if (!bRuntimeMode && !bDiskMode)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UnrealBridgePerf: GetAudioMemoryBreakdown bad mode '%s' — expected disk | runtime"),
			*Mode);
		return Out;
	}

	TMap<FString, FBridgePerfBreakdownRow> Buckets;
	Buckets.Reserve(32);

	if (bRuntimeMode)
	{
		for (TObjectIterator<USoundWave> It; It; ++It)
		{
			USoundWave* SW = *It;
			if (!SW || !IsValid(SW)) continue;
			const int64 Bytes = static_cast<int64>(SW->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal));
			const FString Key = BridgePerfImpl::GetAudioGroupKeyFromObject(SW, ModeEnum);
			BridgePerfImpl::AccumulateBucket(Buckets, Key, Bytes, SW->GetPathName());
		}
	}
	else
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> Assets;
		AR.GetAssetsByClass(USoundWave::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
		for (const FAssetData& Data : Assets)
		{
			if (!Data.IsValid()) continue;
			const int64 Bytes = BridgePerfImpl::GetPackageDiskSize(Data.PackageName);
			if (Bytes <= 0) continue;
			const FString Key = BridgePerfImpl::GetAudioGroupKeyFromAssetData(Data, ModeEnum);
			BridgePerfImpl::AccumulateBucket(Buckets, Key, Bytes, Data.GetSoftObjectPath().ToString());
		}
	}

	Out.Reserve(Buckets.Num());
	for (const TPair<FString, FBridgePerfBreakdownRow>& Pair : Buckets)
	{
		Out.Add(Pair.Value);
	}
	BridgePerfImpl::FinalizeBreakdownRowsByBytes(Out, MaxGroups);
	return Out;
}

// ─── Asset size top-N (M1-6) ────────────────────────────────

namespace BridgePerfImpl
{
	/** Resolve a class filter string to a UClass + the AR query class path.
	 *  Returns true when the filter is empty (no filter — caller should walk
	 *  every asset) or when a class was resolved. Sets OutClassPath to the
	 *  matching class's TopLevelAssetPath; OutClass may be null if we got a
	 *  short-name string that we couldn't resolve to a UClass at runtime. */
	static bool ResolveClassFilter(
		const FString& Filter,
		FTopLevelAssetPath& OutClassPath,
		UClass*& OutClass)
	{
		OutClass = nullptr;
		OutClassPath = FTopLevelAssetPath();

		const FString Trimmed = Filter.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return true; // no filter
		}

		// Full path "/Script/Engine.Texture2D" or "/Game/.../BP_Foo.BP_Foo_C".
		if (Trimmed.StartsWith(TEXT("/")))
		{
			const FTopLevelAssetPath Parsed(Trimmed);
			if (Parsed.IsValid())
			{
				OutClassPath = Parsed;
				OutClass = FindObject<UClass>(nullptr, *Trimmed);
				return true;
			}
			return false;
		}

		// Short name — try resolving as a UClass via FindFirstObject.
		// (UE 5.1+ deprecated FindObject<UClass>(ANY_PACKAGE, ...); the modern
		// API is FindFirstObject which is available 5.3+.)
		if (UClass* Cls = FindFirstObject<UClass>(*Trimmed, EFindFirstObjectOptions::EnsureIfAmbiguous))
		{
			OutClass = Cls;
			OutClassPath = Cls->GetClassPathName();
			return true;
		}

		// Couldn't resolve — agent passed a typo. Refuse to walk the entire
		// AssetRegistry pretending the filter applied.
		return false;
	}
}

TArray<FBridgePerfBreakdownRow> UUnrealBridgePerfLibrary::GetAssetSizeTopN(
	const FString& ClassFilter,
	int32 TopN)
{
	TArray<FBridgePerfBreakdownRow> Out;
	const int32 ClampedTopN = FMath::Clamp(TopN, 1, 1000);

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	TArray<FAssetData> Assets;
	const FString TrimmedFilter = ClassFilter.TrimStartAndEnd();
	if (TrimmedFilter.IsEmpty())
	{
		// All assets. Use a wide-open ARFilter rather than walking every
		// known class — the AR has a single internal call for this.
		FARFilter Filter;
		Filter.bIncludeOnlyOnDiskAssets = true;
		Filter.bRecursivePaths = true;
		Filter.PackagePaths.Add(FName(TEXT("/")));
		Filter.bRecursiveClasses = true;
		AR.GetAssets(Filter, Assets);
	}
	else
	{
		FTopLevelAssetPath ClassPath;
		UClass* Cls = nullptr;
		if (!BridgePerfImpl::ResolveClassFilter(TrimmedFilter, ClassPath, Cls))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UnrealBridgePerf: GetAssetSizeTopN can't resolve class_filter '%s' — ")
				TEXT("expected empty, a /Script/Module.Class path, or a known short class name"),
				*ClassFilter);
			return Out;
		}
		// Subclasses included by default — UTexture sweeps Texture2D / TextureCube / etc.
		AR.GetAssetsByClass(ClassPath, Assets, /*bSearchSubClasses=*/true);
	}

	// Build per-asset rows, then top-N by TotalBytes.
	Out.Reserve(FMath::Min(Assets.Num(), 4096));
	for (const FAssetData& Data : Assets)
	{
		if (!Data.IsValid()) continue;
		const int64 Bytes = BridgePerfImpl::GetPackageDiskSize(Data.PackageName);
		if (Bytes <= 0) continue;

		FBridgePerfBreakdownRow Row;
		Row.Key = Data.GetSoftObjectPath().ToString();
		Row.Count = 1;
		Row.TotalBytes = Bytes;
		Row.LevelName = Data.AssetClassPath.ToString(); // class path (overload)
		Out.Add(MoveTemp(Row));
	}
	BridgePerfImpl::FinalizeBreakdownRowsByBytes(Out, ClampedTopN);
	return Out;
}
