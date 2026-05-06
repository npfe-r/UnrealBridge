#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UnrealBridgePerfLibrary.generated.h"

/**
 * Frame timing snapshot in milliseconds. Values come from the raw
 * GGameThreadTime / GRenderThreadTime / RHIGetGPUFrameCycles globals (updated
 * every frame by FViewport::Draw). When `stat unit` is enabled on the active
 * level viewport, the smoothed FStatUnitData values override the raw ones.
 * Fps is GAverageFPS.
 */
USTRUCT(BlueprintType)
struct FBridgeFrameTiming
{
	GENERATED_BODY()

	/** Engine's running-average FPS (GAverageFPS). 0 before the first full frame. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float Fps = 0.f;

	/** Frame time in ms. FStatUnitData::FrameTime when smoothed; else 1000/Fps. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float FrameMs = 0.f;

	/** Game-thread time, ms. Raw GGameThreadTime, or smoothed FStatUnitData value. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GameThreadMs = 0.f;

	/** Render-thread time, ms. Raw GRenderThreadTime, or smoothed FStatUnitData value. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float RenderThreadMs = 0.f;

	/** GPU frame time, ms. Summed across MAX_NUM_GPUS via RHIGetGPUFrameCycles. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GpuMs = 0.f;

	/** RHI translation time, ms. 0 on the raw path — only set when bSmoothed=true. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float RhiMs = 0.f;

	/** Delta seconds reported by FApp for the most recent frame. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DeltaSeconds = 0.f;

	/** GFrameCounter value at capture time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FrameNumber = 0;

	/** True when `stat unit` is active and values came from FStatUnitData (smoothed). */
	/** False means raw per-frame globals (no running average). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSmoothed = false;
};

/**
 * Per-frame draw call and primitive counters sampled from the RHI globals.
 * These values are the counts for the most recently submitted frame — pulling
 * them once per second is fine; pulling several times per frame returns
 * near-identical numbers.
 */
USTRUCT(BlueprintType)
struct FBridgeRenderCounters
{
	GENERATED_BODY()

	/** GNumDrawCallsRHI summed across MAX_NUM_GPUS. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 DrawCalls = 0;

	/** GNumPrimitivesDrawnRHI summed across MAX_NUM_GPUS. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PrimitivesDrawn = 0;

	/** GNumExplicitGPUsForRendering — usually 1 on desktop builds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 NumGpus = 1;
};

/** Process + platform memory snapshot, in megabytes (MiB). */
USTRUCT(BlueprintType)
struct FBridgeMemoryStats
{
	GENERATED_BODY()

	/** Process working set (FPlatformMemoryStats::UsedPhysical), MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 UsedPhysicalMb = 0;

	/** Process virtual commit (UsedVirtual), MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 UsedVirtualMb = 0;

	/** Peak working set observed this session, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 PeakUsedPhysicalMb = 0;

	/** Peak virtual commit observed this session, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 PeakUsedVirtualMb = 0;

	/** System-wide available physical memory at capture time, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 AvailablePhysicalMb = 0;

	/** System-wide available virtual memory at capture time, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 AvailableVirtualMb = 0;

	/** Total physical RAM on the machine, MiB. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalPhysicalMb = 0;
};

/** Histogram entry: "this class has N live UObjects". */
USTRUCT(BlueprintType)
struct FBridgeUObjectClassCount
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ClassName;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Count = 0;
};

/**
 * UObject population snapshot. Iterates TObjectIterator<UObject> and aggregates
 * by UClass, returning the top-N classes by count. O(N) in the number of live
 * UObjects (typically 100k-2M in an editor session) — don't call on a hot path.
 */
USTRUCT(BlueprintType)
struct FBridgeUObjectStats
{
	GENERATED_BODY()

	/** Total number of live UObjects walked. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 TotalObjects = 0;

	/** Number of distinct UClass types seen. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 UniqueClasses = 0;

	/** Top classes by live-object count, descending. Capped at the caller's TopN. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgeUObjectClassCount> TopClasses;
};

/**
 * Aggregated row for memory / asset / actor breakdown queries on
 * `UUnrealBridgePerfLibrary`. `Key` is the group identifier (folder path,
 * class name, level name, compression format, etc. — depends on which
 * UFUNCTION returned the row). `LevelName` is optional and only populated by
 * actor-breakdown variants that need a per-level disambiguator on top of the
 * primary key (group_by="level_class" → Key=class, LevelName=level).
 */
USTRUCT(BlueprintType)
struct FBridgePerfBreakdownRow
{
	GENERATED_BODY()

	/** Primary group key. Interpretation is per-UFUNCTION (see callers). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Key;

	/** Number of assets / actors / objects falling into this group. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Count = 0;

	/** Total bytes attributed to this group (disk size or runtime size, see caller). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalBytes = 0;

	/** Up to 3 representative paths for "show me what's in here" UI affordance. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> SamplePaths;

	/**
	 * Optional secondary key. Empty for asset-only breakdowns; populated by
	 * level-aware actor breakdowns when the primary key isn't already a level
	 * (e.g. group_by="level_class" returns Key=class, LevelName=level).
	 */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString LevelName;
};

/** Bundled perf snapshot returned by GetPerfSnapshot. */
USTRUCT(BlueprintType)
struct FBridgePerfSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeFrameTiming Timing;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeRenderCounters Render;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeMemoryStats Memory;

	/** Populated only when bIncludeUObjectStats was true. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FBridgeUObjectStats UObjects;

	/** ISO-8601 UTC timestamp for delta-comparison across snapshots. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString CaptureTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString EngineVersion;

	/** True when PIE was active at capture time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bWasInPie = false;
};

/**
 * Structured performance snapshots for UnrealBridge. Replaces parsing
 * `stat unit` text output. All values are read from engine globals + platform
 * APIs on the GameThread.
 */
UCLASS()
class UNREALBRIDGE_API UUnrealBridgePerfLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Return smoothed frame timing (FPS, game/render/GPU ms). Prefers
	 * FStatUnitData from the active editor viewport (smoothed running
	 * average). Falls back to raw RenderCore / RHI globals when no viewport
	 * client is reachable (e.g. headless commandlet). Timings are stale when
	 * the editor hasn't rendered recently — check `frame_number` to detect
	 * this.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeFrameTiming GetFrameTiming();

	/**
	 * Return draw call and primitive counts for the most recent rendered
	 * frame, summed across MAX_NUM_GPUS. 0 on headless builds or before the
	 * first frame.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeRenderCounters GetRenderCounters();

	/** Return process + system memory stats in MiB. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeMemoryStats GetMemoryStats();

	/**
	 * Return the top-N UClass types by live UObject count. Iterates every
	 * live UObject (~100k-2M typical) — expect 10-200 ms. TopN clamped to
	 * [1, 200].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeUObjectStats GetUObjectStats(int32 TopN = 20);

	/**
	 * Aggregate snapshot. bIncludeUObjectStats defaults off because the
	 * UObject iteration is the slow part; pass true when you want the
	 * histogram, false for a cheap per-second sampler.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfSnapshot GetPerfSnapshot(bool bIncludeUObjectStats = false, int32 UObjectTopN = 20);

	/**
	 * Aggregate the editor world's actors by class / level / level_class. Only
	 * walks `World->GetLevels()` — the persistent level plus loaded streaming
	 * sublevels. World Partition projects will report only currently-loaded
	 * actors (the partition unloaded-desc enumeration is a TODO; it requires a
	 * separate API path that has churned across 5.3-5.7). `LevelFilter` is a
	 * substring matched against each level's package short name; empty means
	 * "all levels". `GroupBy` ∈ {"class", "level", "level_class"}. `MaxGroups`
	 * clamped to [1, 1000]; rows sorted by Count descending, ties broken by Key.
	 *
	 * Per-row schema:
	 *   - GroupBy="class":         Key=actor class FName,    LevelName=""
	 *   - GroupBy="level":         Key=level package short,   LevelName=""
	 *   - GroupBy="level_class":   Key=actor class FName,     LevelName=level package short
	 * TotalBytes is always 0 here (actors are runtime-only, no on-disk size).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetWorldActorBreakdown(
		const FString& LevelFilter,
		const FString& GroupBy = TEXT("class"),
		int32 MaxGroups = 200);

	/**
	 * Aggregate UTexture assets by group with disk or runtime byte totals.
	 *
	 * `GroupBy` ∈ {"folder", "lod_group", "compression_format", "sampler_type"}:
	 *   - "folder" → Key = leading content path (e.g. "/Game/Characters")
	 *   - "lod_group" → Key = TextureGroup enum (e.g. "TEXTUREGROUP_World")
	 *   - "compression_format" → Key = TextureCompressionSettings enum (e.g. "TC_Default")
	 *   - "sampler_type" → Key = derived sampler type (e.g. "Color", "Normal", "Masks")
	 *
	 * `Mode` ∈ {"disk", "runtime"} (default "disk"):
	 *   - "disk" walks AssetRegistry without loading textures; reads the package
	 *     file size on disk via FPackageName::DoesPackageExist + IFileManager.
	 *     Never-saved assets have empty TagsAndValues and are skipped.
	 *   - "runtime" iterates loaded UTexture objects via TObjectIterator and
	 *     calls GetResourceSizeBytes(EstimatedTotal). Reflects only currently
	 *     loaded textures — large parts of the project will be invisible.
	 *
	 * `MaxGroups` clamped to [1, 1000]; rows sorted by TotalBytes descending,
	 * ties broken by Count then Key.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetTextureMemoryBreakdown(
		const FString& GroupBy,
		const FString& Mode = TEXT("disk"),
		int32 MaxGroups = 50);

	/**
	 * Aggregate static + skeletal mesh assets by group with disk or runtime
	 * byte totals.
	 *
	 * `GroupBy` ∈ {"folder", "lod_count", "vertex_count_bucket"}:
	 *   - "folder" → Key = leading content path (one level deep)
	 *   - "lod_count" → Key = number of LODs (e.g. "3 LODs")
	 *   - "vertex_count_bucket" → Key = log-scale bucket
	 *     ("<1k", "1k-10k", "10k-100k", "100k-1M", ">=1M")
	 *
	 * `MeshType` ∈ {"static", "skeletal", "all"} (default "all"):
	 *   - "static" walks UStaticMesh only
	 *   - "skeletal" walks USkeletalMesh only
	 *   - "all" walks both, summed into a single bucket per Key
	 *
	 * `Mode` ∈ {"disk", "runtime"} (default "disk"):
	 *   - "disk" reads AssetRegistry tags (LODs, Vertices) without LoadObject;
	 *     bytes come from the package's on-disk file size.
	 *   - "runtime" iterates loaded UStaticMesh / USkeletalMesh objects and
	 *     calls GetResourceSizeBytes(EstimatedTotal); LOD/vertex counts come
	 *     from RenderData. Misses unloaded meshes by design.
	 *
	 * `MaxGroups` clamped to [1, 1000]; rows sorted by TotalBytes descending,
	 * ties broken by Count then Key. Never-saved assets (empty TagsAndValues
	 * + missing on-disk file) are skipped in disk mode.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetMeshMemoryBreakdown(
		const FString& GroupBy,
		const FString& MeshType = TEXT("all"),
		const FString& Mode = TEXT("disk"),
		int32 MaxGroups = 50);

	/**
	 * Top-N UClass histogram with byte totals — runtime-only. Per-class:
	 * Key=class FName, Count=live instance count, TotalBytes=sum of
	 * `GetResourceSizeBytes(EResourceSizeMode::Exclusive)` across instances.
	 * SamplePaths holds up to 3 representative live object paths.
	 *
	 * Iterates every live UObject (`TObjectIterator<UObject>`); typical
	 * editor session is 100k-2M objects so expect 50-300 ms. `TopN` clamped
	 * to [1, 200]; rows sorted by TotalBytes descending. Disk mode is not
	 * meaningful for this query (UObjects are runtime-side) — there is no
	 * `mode` parameter.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetUObjectMemoryBreakdown(int32 TopN = 20);
};
