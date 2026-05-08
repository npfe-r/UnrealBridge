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

/**
 * One bucket from the always-on frame-time histogram. The histogram is
 * accumulated by `BridgePerfFrameHook` (registered at module startup, hooked
 * onto FCoreDelegates::OnEndFrame). `GetFrameTimeHistogram` re-aggregates the
 * fine internal buckets (0.5 ms each) into the caller's coarser buckets.
 */
USTRUCT(BlueprintType)
struct FBridgeHistogramBucket
{
	GENERATED_BODY()

	/** Bucket lower edge in milliseconds, inclusive. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float LowerMs = 0.f;

	/** Bucket upper edge in milliseconds, exclusive. FLT_MAX for the overflow bucket. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float UpperMs = 0.f;

	/** Count of frames whose total time fell into [LowerMs, UpperMs). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Count = 0;

	/** Pre-computed fraction of total observed frames in this bucket, [0, 1]. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float Percent = 0.f;
};

/**
 * One captured hitch (frame whose total time exceeded the caller's threshold).
 * Logged by `BridgePerfFrameHook` into a small ring buffer; `GetHitchLog`
 * filters by threshold and returns the most recent entries.
 */
USTRUCT(BlueprintType)
struct FBridgeHitchEntry
{
	GENERATED_BODY()

	/** GFrameCounter value at hitch capture time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FrameNumber = 0;

	/** FApp::GetCurrentTime() at hitch capture time, in seconds since launch. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TimestampSeconds = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GameThreadMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float RenderThreadMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float GpuMs = 0.f;

	/** Total wall-clock frame time, ms. (Sourced from FApp::GetCurrentTime() delta.) */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float TotalMs = 0.f;
};

/**
 * Status of the opt-in periodic perf sampler. Returned by
 * GetPerfSamplingState. When `bActive` is false the other fields describe the
 * most recently completed run (cleared on next StartPerfSampling).
 */
USTRUCT(BlueprintType)
struct FBridgePerfSamplingState
{
	GENERATED_BODY()

	/** True between StartPerfSampling and StopPerfSampling. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bActive = false;

	/** ISO-8601 UTC timestamp of the last StartPerfSampling call (empty if never started). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString StartedAtUtc;

	/** Currently buffered sample count (resets on each StartPerfSampling). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 SamplesCollected = 0;

	/** Sampler period in milliseconds (as configured at the last StartPerfSampling). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PeriodMs = 0;

	/** Configured ring buffer cap for the active run. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaxSamples = 0;

	/** True when the ticker is also recording UObject stats per sample. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bIncludeUObjectStats = false;
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
 * One material → set of primitives using it (M3-1). Returned by
 * `GetVisiblePrimitivesByMaterial`. The implementation is GT-only — it reads
 * `UPrimitiveComponent::GetUsedMaterials` for every component in the editor
 * world. This means the row reflects "what materials are referenced in this
 * world" rather than "what materials are submitted in the current frame's
 * culled view" — the latter would require RT-side `FScene` traversal which is
 * intentionally avoided here to dodge the cross-version shim cost.
 */
USTRUCT(BlueprintType)
struct FBridgeMaterialRenderRow
{
	GENERATED_BODY()

	/** Asset path of the material / material instance. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString MaterialPath;

	/** Number of distinct UPrimitiveComponents that reference this material. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PrimitiveCount = 0;

	/** Total estimated triangle count contributed across those primitives.
	 *  Static meshes use LOD0 triangle count from the asset RenderData; skeletal
	 *  meshes use LOD0 triangle count from FSkeletalMeshRenderData. 0 when the
	 *  asset is unavailable (e.g. unloaded). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalTriangles = 0;

	/** First 3 actor paths owning a primitive that uses this material. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> SampleActorPaths;
};

/**
 * Per-actor render cost summary (M3-2). Returned by `GetActorRenderCost`
 * and `GetShadowCasterBreakdown`. GameThread-only — reads cached
 * UPrimitiveComponent properties; does not reflect culling state.
 */
USTRUCT(BlueprintType)
struct FBridgeActorRenderCost
{
	GENERATED_BODY()

	/** Actor path (e.g. "/Game/Maps/Forest.Forest:PersistentLevel.SM_Tree_42"). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ActorPath;

	/** Number of UPrimitiveComponents on the actor (visible or hidden). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PrimitiveComponentCount = 0;

	/** Sum of `GetNumMaterials()` across primitive components. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaterialSlotCount = 0;

	/** Sum of LOD0 triangle counts across static + skeletal mesh components.
	 *  Returns 0 for primitive types we don't recognize (particle systems, etc.). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 EstimatedTriangleCount = 0;

	/** True when ANY primitive component on the actor has bCastDynamicShadow=true. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bCastsDynamicShadow = false;

	/** Distinct material asset paths referenced across all primitives. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> Materials;
};

/**
 * Lumen runtime diagnostics (M3-4). Populated only on UE 5.7+. On older
 * versions every field stays at its default-zero value and the UFUNCTION's
 * stub logs a one-time warning. Public Lumen state in 5.7 is mostly
 * visualization metadata — the surface-cache / probe counters live in the
 * Renderer module's private `FLumenSceneData` and aren't reachable from a
 * non-Renderer module. We expose what the engine API surfaces publicly:
 * available visualization modes (proxy for "Lumen feature is wired up") and
 * the active mode name (resolved via the `r.Lumen.Visualize.ViewMode` CVar).
 */
USTRUCT(BlueprintType)
struct FBridgeLumenDiagnostics
{
	GENERATED_BODY()

	/** True when GetLumenVisualizationData() returned an initialized instance.
	 *  False on unsupported engine versions or when the visualization data
	 *  module hasn't been loaded yet. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bAvailable = false;

	/** Engine identifier returned by FEngineVersion::Current() — handy for
	 *  attributing data when comparing snapshots across versions. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString EngineVersion;

	/** All visualization mode names registered with FLumenVisualizationData
	 *  (e.g. "Overview", "FinalLightingScene"). Empty when bAvailable=false. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> VisualizationModes;

	/** Currently active visualization mode according to r.Lumen.Visualize.ViewMode.
	 *  Empty string when no mode is active or the CVar isn't registered. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ActiveVisualizationMode;

	/** True when r.DynamicGlobalIlluminationMethod is set to a value that
	 *  enables Lumen GI. Determined at call time from the CVar. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bLumenGiEnabled = false;

	/** True when r.ReflectionMethod is set to a value that enables Lumen
	 *  reflections. Determined at call time from the CVar. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bLumenReflectionsEnabled = false;
};

/**
 * Nanite runtime stats (M3-5). Populated only on UE 5.7+. Reads the public
 * surface of `Nanite::GStreamingManager` (an ENGINE_API global) — capacity
 * configuration the streaming manager exposes via its public getters. Nanite's
 * per-frame cluster / visible-set counters live in private members and aren't
 * accessible without Renderer module access; we record them as 0 with a note
 * in the field comments. Non-zero default values indicate Nanite is at least
 * configured even without per-frame telemetry.
 */
USTRUCT(BlueprintType)
struct FBridgeNaniteStats
{
	GENERATED_BODY()

	/** True when GStreamingManager has at least one resource entry (HasResourceEntries()).
	 *  False on unsupported engine versions or when no Nanite mesh has loaded. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bAvailable = false;

	/** Nanite::GStreamingManager.GetMaxStreamingPages() — total streaming-page
	 *  budget the manager was initialized with. Independent of how many are
	 *  resident this frame. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaxStreamingPages = 0;

	/** Nanite::GStreamingManager.GetMaxHierarchyLevels(). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaxHierarchyLevels = 0;

	/** True when the streaming manager reported IsSafeForRendering() at call
	 *  time. Useful as a "Nanite system actually initialized" check. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bIsSafeForRendering = false;

	/** Estimated count of UStaticMesh assets in the editor world whose
	 *  IsNaniteEnabled() / HasValidNaniteData() returns true. Aggregated on
	 *  the GameThread by walking UStaticMeshComponents — does NOT require
	 *  RT sync. 0 on unsupported engine versions. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 NaniteStaticMeshComponents = 0;

	/** Engine version string for snapshot attribution. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString EngineVersion;
};

/**
 * Result of `StartTraceCapture` (M4-1). When `bSuccess` is true, `Path` is
 * the absolute file path the trace is being written to and `Channels` is the
 * channel set the engine reports as active. On failure, `Error` describes the
 * cause (already-active capture, FTraceAuxiliary::Start returned false, etc.)
 * and `Path` / `Channels` are empty.
 */
USTRUCT(BlueprintType)
struct FBridgeTraceStartResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSuccess = false;

	/** Absolute path of the .utrace file being written (empty on failure). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Path;

	/** Comma-separated channel list passed to the trace API, split into entries. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> Channels;

	/** Diagnostic message; empty on success. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Error;
};

/**
 * Result of `StopTraceCapture` (M4-2). `Path` is the file we *started* writing
 * to (the engine API doesn't always echo the actual write target back); the
 * caller can stat that path to confirm size > 0. `DurationSeconds` is wall
 * clock time between the matching start/stop pair. `SizeBytes` is the file
 * size the OS reports at stop time — 0 if the trace file isn't on disk yet
 * (file system flush delay) or never started.
 */
USTRUCT(BlueprintType)
struct FBridgeTraceStopResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 SizeBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DurationSeconds = 0.f;
};

/**
 * Snapshot of the trace capture state (M4-3). Returned by `GetTraceState`.
 * When `bActive` is false the other fields describe the most recent run (or
 * are zeroed if no run ever happened). `CurrentSizeBytes` queries the file
 * system at call time — it is monotonic during a live capture.
 */
USTRUCT(BlueprintType)
struct FBridgeTraceState
{
	GENERATED_BODY()

	/** True between StartTraceCapture and StopTraceCapture (or until the
	 *  engine's FTraceAuxiliary reports the capture is no longer connected). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bActive = false;

	/** Absolute path the active (or last) capture is writing to. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Path;

	/** Live file size of the active trace file at query time, in bytes.
	 *  0 when not active or file not yet flushed. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 CurrentSizeBytes = 0;

	/** Channel set the active (or last) capture was started with. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> Channels;

	/** ISO-8601 UTC timestamp of the matching StartTraceCapture (empty if
	 *  no run has been started yet). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString StartedAtUtc;
};

/**
 * One row from the trace channel registry (M4-4). Returned by
 * `ListTraceChannels`. `Description` is populated only when the engine's
 * EnumerateChannels overload exposes it; on the older callback shape the
 * field stays empty.
 */
USTRUCT(BlueprintType)
struct FBridgeTraceChannelInfo
{
	GENERATED_BODY()

	/** Channel name (e.g. "cpu", "gpu", "frame", "rdg"). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Name;

	/** True when the channel is currently emitting events. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bEnabled = false;

	/** Engine-supplied description; may be empty on older versions. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Description;
};

/** One row of `FBridgePerfTraceSummary::HotScopes` (M4-5). */
USTRUCT(BlueprintType)
struct FBridgePerfHotScope
{
	GENERATED_BODY()

	/** Timer name as registered via UE_TRACE_CPU_PROFILER_EVENT_SCOPE / TRACE_CPUPROFILER_EVENT_SCOPE. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Name;

	/** Total inclusive time across every invocation in the trace, in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TotalMs = 0.0;

	/** Number of times this timer scope opened during the trace. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 CallCount = 0;
};

/** Result of `begin_insights_for_trace` (M8-3). */
USTRUCT(BlueprintType)
struct FBridgeInsightsLaunchResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSuccess = false;

	/** Absolute path to UnrealInsights.exe that was launched (echoed back). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString InsightsExePath;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString TracePath;

	/** OS process id of the launched Insights instance (0 on failure). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 ProcessId = 0;

	/** Failure diagnostic when bSuccess=false. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Error;
};

/**
 * Per-field delta between two `FBridgePerfSnapshot` instances (M8-2).
 * Convention: every Delta field is `After - Before`. Positive memory deltas
 * mean "after used more", positive timing deltas mean "after was slower".
 */
USTRUCT(BlueprintType)
struct FBridgePerfSnapshotDelta
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString BeforeTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString AfterTimeUtc;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DeltaFps = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DeltaFrameMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DeltaGameThreadMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DeltaRenderThreadMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float DeltaGpuMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 DeltaDrawCalls = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 DeltaPrimitivesDrawn = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 DeltaUsedPhysicalMb = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 DeltaUsedVirtualMb = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 DeltaPeakUsedPhysicalMb = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 DeltaAvailablePhysicalMb = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 DeltaTotalObjects = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 DeltaUniqueClasses = 0;

	/** True when at least one significant regression was detected (>= threshold). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSignificantRegression = false;

	/** Human-readable list of regressions worth flagging. Each entry names
	 *  one metric, the before/after values and the % change. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FString> Regressions;
};

/**
 * One trace counter (M5-3). Sourced from `ICounterProvider::EnumerateCounters`
 * + per-counter `EnumerateValues` / `EnumerateFloatValues`. Values are
 * aggregated over the full session interval; counter ranges (min / max /
 * avg / last / sum) are computed on the fly without materialising the
 * full timeline.
 *
 * Common UE counters (non-exhaustive): "FrameTime", "GameThreadTime",
 * "TotalGCBytesAllocated", any `TRACE_INT_VALUE` / `TRACE_FLOAT_VALUE`
 * the running game emits, plus a long tail of stat-derived counters.
 */
USTRUCT(BlueprintType)
struct FBridgePerfCounter
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Name;

	/** Group name as registered (e.g. "Memory", "Stats", or empty). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Group;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Description;

	/** True for `TRACE_FLOAT_VALUE`; false for `TRACE_INT_VALUE`. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bFloatingPoint = false;

	/** True for stats-style counters that are reset to 0 every frame. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bResetEveryFrame = false;

	/** Number of samples observed across the trace. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 SampleCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double MinValue = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double MaxValue = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double SumValue = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double AverageValue = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double LastValue = 0.0;
};

/**
 * One package's load-time cost (M5-5). Sourced from
 * `ILoadTimeProfilerProvider::CreatePackageDetailsTable` — the same table
 * Insights' "Asset Loading Insights" tab walks. Times are seconds-as-double
 * converted to ms for consistency with the rest of the perf library.
 */
USTRUCT(BlueprintType)
struct FBridgePerfLoadTimeRow
{
	GENERATED_BODY()

	/** Package name as recorded in the trace (e.g. "/Game/Maps/Forest"). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString PackageName;

	/** Sum of MainThreadTime + AsyncLoadingThreadTime, in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TotalLoadingMs = 0.0;

	/** Time spent loading this package on the main thread, ms. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double MainThreadMs = 0.0;

	/** Time spent on the async loading thread, ms. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double AsyncLoadingThreadMs = 0.0;

	/** Total bytes serialized for this package (`TotalSerializedSize`). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 SerializedSizeBytes = 0;

	/** Header bytes (`SerializedHeaderSize`). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 HeaderSizeBytes = 0;

	/** Export-table bytes (`SerializedExportsSize`). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 ExportsSizeBytes = 0;

	/** Number of exports recorded under this package. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ExportCount = 0;
};

/**
 * One thread's top-N hot scopes (M5-2). Walked by `ParseTraceToSummary` for
 * every CPU thread the IThreadProvider reports — `GameThread`, `RenderThread`,
 * `RHIThread`, every `WorkerThread N`, etc. Each row's `TopScopes` is capped
 * at the caller-provided TopN_PerThread to keep memory bounded on large traces.
 */
USTRUCT(BlueprintType)
struct FBridgePerThreadHotScopes
{
	GENERATED_BODY()

	/** OS thread name as registered with the trace stream. Empty if the
	 *  thread emitted timing events but never registered a name. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ThreadName;

	/** Engine thread group ("GameThread" / "RenderingThread" / "Foreground" /
	 *  empty for unknown). Source: `FThreadInfo::GroupName`. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString GroupName;

	/** OS thread id. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 ThreadId = 0;

	/** Sum of every CPU scope's inclusive time on this thread, in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TotalCpuMs = 0.0;

	/** Top-N timer scopes on this thread, ranked by total inclusive time desc. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfHotScope> TopScopes;
};

/** Result of `parse_trace_to_summary` (M4-5). */
USTRUCT(BlueprintType)
struct FBridgePerfTraceSummary
{
	GENERATED_BODY()

	/** Path that was analysed (echoed back for log/debug). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString TracePath;

	/** Size of the .utrace file on disk in bytes. 0 if missing. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FileSizeBytes = 0;

	// ─── Diagnostics (from FSessionInfo) ─────────────────────────

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Platform;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString AppName;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ProjectName;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString BuildVersion;

	/** Source-control changelist; 0 if unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Changelist = 0;

	// ─── Frame timing aggregate (Game frames only) ───────────────

	/** Sum of (EndTime - StartTime) over all Game frames, in seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TotalDurationSeconds = 0.0;

	/** Number of Game frames in the trace. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 GameFrameCount = 0;

	/** Mean frame duration, in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float FrameAvgMs = 0.f;

	/** Fastest frame, in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float FrameMinMs = 0.f;

	/** Slowest frame (worst hitch), in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float FrameMaxMs = 0.f;

	// ─── Hot scopes (CPU TimingProfiler aggregate) ───────────────

	/** Top-N timers ranked by total inclusive time across all CPU thread timelines. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfHotScope> HotScopes;

	// ─── M5-1: GPU hot scopes ────────────────────────────────────

	/** Top-N GPU timer scopes by total inclusive time, aggregated across every
	 *  GPU queue (`ITimingProfilerProvider::EnumerateGpuQueues`). Empty when
	 *  the trace did not include the `gpu` channel. Names come from the same
	 *  timer table as CPU scopes — GPU timers carry `ETimingProfilerTimerType::GpuScope`. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfHotScope> GpuHotScopes;

	// ─── M5-2: per-thread hot scopes ─────────────────────────────

	/** One row per CPU thread the trace recorded events on. Each row carries
	 *  the thread's name + group + total CPU time + top-N hottest scopes on
	 *  that thread. Use this to distinguish GameThread vs RenderingThread vs
	 *  WorkerThread bottlenecks. Sorted by `TotalCpuMs` descending. Cap of
	 *  256 KB per thread is enforced via `TopNPerThread` clamp. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerThreadHotScopes> PerThreadHotScopes;

	// ─── M5-3: counters ──────────────────────────────────────────

	/** Active trace counters with min/max/avg/last/sum aggregates over the
	 *  full session interval. Ranked by `SampleCount` descending — busier
	 *  counters surface first. Capped by `TopNCounters`; pass 0 at call time
	 *  to skip the counter pass entirely. Counters with 0 samples are
	 *  omitted. Empty when the trace did not include the `counter` channel. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfCounter> Counters;

	// ─── M5-5: load-time breakdown ───────────────────────────────

	/** Top-N packages by total load time across the trace's full interval.
	 *  Sourced from `ILoadTimeProfilerProvider::CreatePackageDetailsTable`.
	 *  Empty when the trace did not include the `loadtime` channel. Sorted
	 *  by `TotalLoadingMs` descending. Use to attribute cold-load 30 s+
	 *  spikes to specific packages. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfLoadTimeRow> LoadTimeBreakdown;

	/** True when Analyze + every provider read succeeded. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSuccess = false;

	/** Populated when `bSuccess=false`: human-readable failure reason. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Error;
};

/** One connection's per-direction traffic totals (M6-2). */
USTRUCT(BlueprintType)
struct FBridgePerfNetConnection
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Name;

	/** Connection address string (typically `IP:port` or local-loopback marker). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString AddressString;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ConnectionId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 IncomingPacketCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 OutgoingPacketCount = 0;

	/** Sum of `TotalPacketSizeInBytes` over every incoming packet. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 IncomingBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 OutgoingBytes = 0;
};

/** One game-instance summary in a net trace (M6-2). */
USTRUCT(BlueprintType)
struct FBridgePerfNetGameInstance
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString InstanceName;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bIsServer = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bIsUsingIrisReplication = false;

	/** Number of replicated objects observed on this instance. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ObjectCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfNetConnection> Connections;
};

/** One streaming-texture row (M7-1). */
USTRUCT(BlueprintType)
struct FBridgeTextureStreamingRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString TexturePath;

	/** Number of LODs (mips) currently resident in GPU memory. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ResidentMipCount = 0;

	/** Number of LODs the streamer wants to be resident after the next update. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 WantedMipCount = 0;

	/** Total LOD count available on the asset. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 MaxMipCount = 0;

	/** Resident GPU memory for this texture, bytes. From `CalcCumulativeLODSize(NumResidentLODs)`. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 ResidentBytes = 0;

	/** Bytes the streamer would consume if all wanted LODs were paged in. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 WantedBytes = 0;

	/** `FApp::GetCurrentTime()` value of the last frame this texture was rendered. FLT_MAX = never (always-resident). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	float LastVisibleSeconds = 0.f;

	/** True when force-resident is set (cinematic / manual override). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bForceResident = false;
};

/**
 * One material's structural-complexity row (M7-4). Reports counts of
 * expressions / samplers / parameters that are derivable from the in-memory
 * material graph without recompiling shaders. Real GPU instruction counts
 * + sampler-slot pressure live behind `FMaterialStatsUtils::ExtractMatertialStatsInfo`
 * which requires the `MaterialEditor` engine module — out of scope for the
 * bridge plugin's lean dependency set.
 */
USTRUCT(BlueprintType)
struct FBridgeMaterialPerfRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString MaterialPath;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString BlendMode;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString ShadingModel;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString MaterialDomain;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bTwoSided = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bUsedWithSkeletalMesh = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bUsedWithStaticLighting = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ExpressionCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 TextureSampleCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 CustomExpressionCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 StaticSwitchCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ScalarParameterCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 VectorParameterCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 TextureParameterCount = 0;

	/** A simple sortable cost proxy: ExpressionCount + 4×TextureSampleCount + 8×CustomExpressionCount.
	 *  Not a real GPU cost — just a heuristic to surface the heaviest graphs at the top. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ComplexityScore = 0;
};

/** Result of `analyze_all_materials` (M7-4). */
USTRUCT(BlueprintType)
struct FBridgeAllMaterialsAnalysis
{
	GENERATED_BODY()

	/** Number of UMaterial master assets walked. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 TotalMaterials = 0;

	/** Number of UMaterialInstance assets discovered (not analysed individually). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 TotalMaterialInstances = 0;

	/** Top-N rows by `ComplexityScore` desc. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgeMaterialPerfRow> Rows;
};

/** One per-pass GPU timing row (M7-3). Microseconds → milliseconds for output. */
USTRUCT(BlueprintType)
struct FBridgeGpuPassTiming
{
	GENERATED_BODY()

	/** Pass description as registered by SCOPED_GPU_STAT etc. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString PassName;

	/** GPU index this measurement was taken on (0 in single-GPU). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 GpuIndex = 0;

	/** Average GPU time over the profiler's history window, ms. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double AverageMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double MinMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double MaxMs = 0.0;
};

/** Result of `get_per_pass_gpu_timings` (M7-3). */
USTRUCT(BlueprintType)
struct FBridgeGpuPassTimings
{
	GENERATED_BODY()

	/** True when the realtime GPU profiler returned data. False on the new
	 *  RHI GPU profiler path (`RHI_NEW_GPU_PROFILER=1`) where the legacy
	 *  FetchPerfByDescription API is empty — fall back to Insights with the
	 *  `gpu` + `rdg` channels. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bAvailable = false;

	/** Sum of `AverageMs` across all rows. Approximates last-frame GPU time
	 *  for "scene rendering" stat scopes; not a literal frame total because
	 *  passes can overlap on multi-queue GPUs. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double SumAverageMs = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PassCount = 0;

	/** Pass rows, sorted by `AverageMs` descending. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgeGpuPassTiming> Passes;

	/** Diagnostic message when bAvailable=false. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Diagnostic;
};

/** One render-target entry (M7-2). */
USTRUCT(BlueprintType)
struct FBridgeRenderTargetEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Path;

	/** Class short name: "TextureRenderTarget2D" / "TextureRenderTargetCube" / etc. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString TypeName;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Width = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Height = 0;

	/** Volume / array depth. 1 for 2d, 6 for cube, N for 2d-array, N for volume. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Depth = 1;

	/** Estimated bytes from `GetResourceSizeBytes(EstimatedTotal)`. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 Bytes = 0;
};

/**
 * Result of `get_render_target_memory` (M7-2).
 *
 * Walks `UTextureRenderTarget*` objects via TObjectIterator. Engine-internal
 * RTs (GBuffer, shadow atlas, Lumen surface cache, virtual-texture pool,
 * lighting cache) live inside the renderer module's private FScene state and
 * are NOT visible from here. This pass surfaces user-created / Blueprint RTs
 * + the editor's CanvasRTs — the things an agent can attribute to its own
 * code. For engine-internal RT memory, use `stat scenerendertargets` /
 * Insights.
 */
USTRUCT(BlueprintType)
struct FBridgeRenderTargetMemory
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 RenderTargetCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 RenderTarget2DBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 RenderTargetCubeBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 RenderTarget2DArrayBytes = 0;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 RenderTargetVolumeBytes = 0;

	/** Top-N entries by `bytes` desc. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgeRenderTargetEntry> Entries;
};

/** Result of `get_texture_streaming_residency` (M7-1). */
USTRUCT(BlueprintType)
struct FBridgeTextureStreamingState
{
	GENERATED_BODY()

	/** True when texture streaming is enabled in the project (`r.TextureStreaming` etc.). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bEnabled = false;

	/** Streaming-pool budget, bytes. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 PoolSizeBytes = 0;

	/** Bytes the streamer would consume if there were no pool limit. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 RequiredPoolBytes = 0;

	/** Positive when the streamer is over its budget. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 MemoryOverBudgetBytes = 0;

	/** Peak required budget observed since the last reset. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 MaxEverRequiredBytes = 0;

	/** Total streaming UTexture2D objects observed during the GT walk. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 NumStreamingTextures = 0;

	/** Top-N rows ranked by `ResidentBytes` desc. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgeTextureStreamingRow> Rows;
};
/** One cooked-package row (M6-3). */
USTRUCT(BlueprintType)
struct FBridgePerfCookRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString PackageName;

	/** Top-level asset class for the package (e.g. "Texture2D", "Material"). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString AssetClass;

	/** Sum of every cooker phase, ms. Use to rank "which packages took longest to cook". */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TotalCookTimeMs = 0.0;

	/** Inclusive `LoadPackage` time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double LoadTimeMs = 0.0;

	/** Inclusive `SavePackage` time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double SaveTimeMs = 0.0;

	/** Inclusive `BeginCacheForCookedPlatformData` time (typically dominated by shader / derived-data cache). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double BeginCacheCookedPlatformDataMs = 0.0;

	/** Inclusive `IsCachedCookedPlatformDataLoaded` poll time. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double IsCachedCookedPlatformDataLoadedMs = 0.0;
};

/** Result of `parse_cook_trace_to_summary` (M6-3). */
USTRUCT(BlueprintType)
struct FBridgePerfCookSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString TracePath;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FileSizeBytes = 0;

	/** True when the cook profiler provider returned ≥ 1 package. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bHasEvents = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 PackageCount = 0;

	/** Sum of every package's `TotalCookTimeMs`, in seconds. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	double TotalCookTimeSeconds = 0.0;

	/** Top-N packages by `TotalCookTimeMs` desc. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfCookRow> Packages;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Error;
};

USTRUCT(BlueprintType)
struct FBridgePerfNetSummary
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString TracePath;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FileSizeBytes = 0;

	/** NetTrace stream version, or 0 when the trace had no network data. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 NetTraceVersion = 0;

	/** True when at least one game instance + non-zero NetTrace version. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bHasEvents = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfNetGameInstance> GameInstances;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Error;
};

/** One LLM tag from the alloc trace (M6-1). */
USTRUCT(BlueprintType)
struct FBridgePerfAllocTag
{
	GENERATED_BODY()

	/** Tag id assigned by the alloc tracer. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 Id = 0;

	/** Parent tag id (-1 for root tags). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int32 ParentId = -1;

	/** Display name (e.g. "Textures", "Audio.Mixer"). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Name;

	/** Full hierarchical path including parent (e.g. "Audio/Mixer/Decoders"). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString FullPath;
};

/**
 * Result of `parse_alloc_trace_to_summary` (M6-1). Aggregates traceServices'
 * IAllocationsProvider over the full session interval. The async per-allocation
 * query path (`EQueryRule::aAf` + StartQuery / PollQuery / NextResult) for
 * top-N unfreed allocations by size+callstack is deferred to M6-1 phase 2 —
 * this MVP delivers timeline aggregates + tag inventory which are already
 * enough to answer "did the run leak", "peak commit", "what LLM tags are
 * registered" without touching the async machinery.
 */
USTRUCT(BlueprintType)
struct FBridgePerfAllocSummary
{
	GENERATED_BODY()

	/** Echoed back. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString TracePath;

	/** OS-reported size of the .utrace file. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 FileSizeBytes = 0;

	/** True when the AllocationsProvider reported any alloc/free/heap events.
	 *  False when the trace lacks the `memalloc` channel — every other field
	 *  will be 0. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bHasEvents = false;

	/** Peak total allocated memory across the trace, in bytes. Sourced from
	 *  the `MaxTotalAllocatedMemory` timeline. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 PeakTotalAllocatedBytes = 0;

	/** Peak number of simultaneously-live allocations. Sourced from the
	 *  `MaxLiveAllocations` timeline. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 PeakLiveAllocations = 0;

	/** Total alloc events the trace recorded (cumulative across all threads). */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalAllocEvents = 0;

	/** Total free events the trace recorded. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 TotalFreeEvents = 0;

	/** Difference: alloc - free. Positive = trace ended with live allocations
	 *  (always true for editor traces — the editor doesn't free everything on
	 *  shutdown). Use compared between two captures of the same workload to
	 *  detect leaks. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	int64 AllocFreeDelta = 0;

	/** All registered LLM tags. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	TArray<FBridgePerfAllocTag> Tags;

	/** True when Analyze + provider read succeeded. */
	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Perf")
	FString Error;
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

	/**
	 * Aggregate USoundWave assets by group with disk or runtime byte totals.
	 *
	 * `GroupBy` ∈ {"compression_format", "folder", "sample_rate_bucket",
	 * "channel_count"}:
	 *   - "compression_format" → Key = ESoundAssetCompressionType enum
	 *     (e.g. "PlatformSpecific", "BinkAudio", "PCM")
	 *   - "folder" → Key = leading content path (one level deep)
	 *   - "sample_rate_bucket" → Key = "<8k", "8k-16k", "16k-22k", "22k-44k",
	 *     "44k-48k", "48k-96k", ">=96k", "(unknown)"
	 *   - "channel_count" → Key = "Mono" / "Stereo" / "5.1" / "7.1" / "Other"
	 *
	 * `Mode` ∈ {"disk", "runtime"} (default "disk"):
	 *   - "disk" walks AssetRegistry without LoadObject; bytes from package
	 *     file size; group keys come from TagsAndValues.
	 *   - "runtime" iterates loaded USoundWave objects.
	 *
	 * `MaxGroups` clamped to [1, 1000]; rows sorted by TotalBytes descending.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetAudioMemoryBreakdown(
		const FString& GroupBy = TEXT("compression_format"),
		const FString& Mode = TEXT("disk"),
		int32 MaxGroups = 50);

	/**
	 * Top-N largest assets on disk, optionally filtered by UClass. Each row
	 * is one asset: Key=full asset path (e.g. "/Game/Foo/Bar.Bar"), Count=1,
	 * TotalBytes=on-disk package size, LevelName=class path of the asset
	 * (e.g. "/Script/Engine.Texture2D"), SamplePaths empty.
	 *
	 * `ClassFilter`:
	 *   - empty → all assets
	 *   - "/Script/Engine.Texture2D" → exact class match (top-level path)
	 *   - "Texture2D" → matched against the asset class short name
	 * Subclasses are included by default — passing UTexture sweeps every
	 * Texture2D / TextureCube / etc.
	 *
	 * `TopN` clamped to [1, 1000]. Walks AssetRegistry only — no LoadObject;
	 * never-saved assets are skipped (their disk contribution is 0).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetAssetSizeTopN(
		const FString& ClassFilter,
		int32 TopN = 50);

	// ─── M2: time series & sampling ────────────────────────────

	/**
	 * Return a histogram of per-frame total time observed since module load
	 * (or since the last `ResetFrameTimeHistogram` call). Frames are recorded
	 * in fine 0.5 ms internal buckets by an OnEndFrame hook that started at
	 * StartupModule; this UFUNCTION re-aggregates them into caller-supplied
	 * `BucketMs`-wide buckets up to `MaxBucketMs`. Frames above `MaxBucketMs`
	 * land in a single overflow bucket whose UpperMs is FLT_MAX.
	 *
	 * `BucketMs` clamped to [0.5, 50.0]; values below 0.5 round up to 0.5
	 * (the internal resolution). `MaxBucketMs` clamped to [BucketMs, 200.0].
	 *
	 * Cost is O(internal_buckets) per call (~400 reads) — safe to call
	 * at any rate. Bucket Percent fields sum to 1.0 across all buckets.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeHistogramBucket> GetFrameTimeHistogram(
		float BucketMs = 5.f,
		float MaxBucketMs = 100.f);

	/**
	 * Return up to `MaxEntries` most-recent hitch entries (frames whose total
	 * time exceeded the OnEndFrame hitch detector threshold) where each
	 * entry's TotalMs is also at or above `ThresholdMs`. Entries are returned
	 * newest-last (chronological order).
	 *
	 * The hitch detector logs every frame above an internal min threshold
	 * (33 ms — i.e. anything below 30 fps); `ThresholdMs` re-filters that
	 * captured set per call, so values below 33 will simply return whatever
	 * was already captured. `MaxEntries` clamped to [1, 200].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeHitchEntry> GetHitchLog(
		float ThresholdMs = 50.f,
		int32 MaxEntries = 50);

	/**
	 * Zero out all internal frame-time histogram buckets so the next
	 * `GetFrameTimeHistogram` returns only frames captured after this call.
	 * Useful to baseline before/after a measured change.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static void ResetFrameTimeHistogram();

	/** Drop every captured hitch entry. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static void ClearHitchLog();

	/**
	 * Compute percentile frame times from the always-on internal histogram (M5-4).
	 *
	 * Returns one ms value per percentile in `Percentiles`, in input order. Each
	 * percentile p is interpreted as a value in [0, 100] (clamped) and resolves
	 * to the smallest frame time T such that at least p% of all observed frames
	 * had total time ≤ T. The resolution within a bucket is linear (sub-bucket
	 * percentile sits proportionally between LowerMs and UpperMs).
	 *
	 * Returns an array of zeros when no frames have been observed yet (or after
	 * `ResetFrameTimeHistogram`). The overflow bucket (frames > 200 ms) reports
	 * its lower edge — exact ms is unknown for those frames.
	 *
	 * Cost: O(buckets + len(Percentiles)) ~= microseconds. Safe to poll.
	 *
	 * Typical use: `get_frame_time_percentiles([50, 90, 95, 99])` for a hitch
	 * survey. p99 is the standard "worst real-world frame" metric — looking at
	 * `frame_max_ms` is misleading because it picks up one-frame outliers.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<float> GetFrameTimePercentiles(const TArray<float>& Percentiles);

	/**
	 * Begin (or restart) periodic perf sampling. A FTSTicker on the GameThread
	 * fires every `PeriodMs` and captures a `FBridgePerfSnapshot` into a
	 * ring buffer of size `MaxSamples`. When `bIncludeUObjectStats` is true,
	 * the UObject-iteration step (50-300 ms typical) runs every tick — only
	 * enable it when paired with periods >= 5000 ms.
	 *
	 * Idempotent: calling while already active discards the prior run's
	 * buffer and restarts with the new parameters.
	 *
	 * `PeriodMs` clamped to [10, 60000]; `MaxSamples` clamped to [1, 10000].
	 * Returns true on success, false only if the FTSTicker registration fails
	 * (extremely unusual — would mean the engine's core ticker is unavailable).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static bool StartPerfSampling(
		int32 PeriodMs = 100,
		int32 MaxSamples = 600,
		bool bIncludeUObjectStats = false);

	/**
	 * Stop sampling and return the buffered snapshots (empty when nothing was
	 * captured or sampling was never started). The internal buffer is
	 * cleared after this call. Calling while inactive is safe and returns
	 * whatever was previously buffered (typically empty after the prior
	 * StopPerfSampling already drained it).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfSnapshot> StopPerfSampling();

	/** Return the active / inactive state of the periodic sampler. */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfSamplingState GetPerfSamplingState();

	/**
	 * Serialize the current periodic-sampling buffer to CSV. The header is:
	 *   timestamp_utc, frame_number, fps, frame_ms, gt_ms, rt_ms, gpu_ms,
	 *   rhi_ms, delta_seconds, used_physical_mb, used_virtual_mb,
	 *   available_physical_mb, draw_calls, primitives_drawn, was_in_pie,
	 *   engine_version
	 *
	 * UObject top-class stats are intentionally omitted (would balloon the row
	 * width). One row per sample. Works whether sampling is active or stopped:
	 * the buffer is read in-place without clearing.
	 *
	 * `OutputPath` resolution:
	 *   - empty → <Project>/Saved/UnrealBridge/perf_samples_<unix>.csv
	 *   - directory → that directory + the auto-named file
	 *   - any other path → used as-is (parent dir created on demand)
	 *
	 * Returns true on successful write, false on I/O error or empty buffer.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static bool ExportPerfSamplesToCsv(const FString& OutputPath);

	// ─── M3: render breakdown ──────────────────────────────────

	/**
	 * Aggregate primitive components by material, returning the top-N rows
	 * by primitive count (ties broken by triangle total). Walks every
	 * UPrimitiveComponent in the editor world on the GameThread; per
	 * component reads `GetUsedMaterials()` (public API, stable 5.3-5.7) and
	 * accumulates the asset path → primitive count + triangle total +
	 * sample-actor list mapping.
	 *
	 * Limitations to advertise to callers:
	 *   - GameThread-only — no RT-side scene traversal. The result reflects
	 *     "what materials are referenced" not "what materials drew this
	 *     frame after culling". For most diagnostic uses this is what you
	 *     want; for true visible-set accounting hook UE Insights instead.
	 *   - `ViewportIndex` is accepted but currently ignored — the editor
	 *     world has one canonical material set regardless of which viewport
	 *     is looking at it. Reserved for a future RT-side enrichment.
	 *   - Triangle counts use LOD0; we don't try to guess current LOD per
	 *     component (would need RT-side screen-size resolution).
	 *
	 * `TopN` clamped to [1, 1000].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeMaterialRenderRow> GetVisiblePrimitivesByMaterial(
		int32 ViewportIndex = 0,
		int32 TopN = 50);

	/**
	 * Per-actor render cost summary. Resolves `ActorPath` via
	 * `FindObject<AActor>` against the editor world (must be a fully
	 * qualified path like "/Game/Maps/Forest.Forest:PersistentLevel.SM_Tree_42").
	 *
	 * Returns an empty struct (ActorPath="" indicating not-found) when the
	 * actor cannot be resolved. Otherwise iterates UPrimitiveComponents on
	 * the actor and aggregates:
	 *   - PrimitiveComponentCount: total primitive components
	 *   - MaterialSlotCount: sum of GetNumMaterials() per primitive
	 *   - EstimatedTriangleCount: sum of LOD0 triangles for static / skel
	 *     mesh components (other primitive types contribute 0)
	 *   - bCastsDynamicShadow: OR of bCastDynamicShadow across primitives
	 *   - Materials: distinct material asset paths referenced
	 *
	 * GameThread-only; no RT sync. Cost is O(num_components_on_actor).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeActorRenderCost GetActorRenderCost(const FString& ActorPath);

	/**
	 * Aggregate static + skeletal mesh components in the editor world by
	 * `<asset_path>:LOD<n>`, returning a breakdown row per (mesh, lod) pair.
	 *
	 * The "current LOD" used here is the component's
	 * `GetForcedLODRequested()` / equivalent best-effort accessor; when the
	 * component has no forced LOD we fall back to LOD0. We intentionally do
	 * NOT compute the screen-size-driven dynamic LOD because that requires
	 * the rendered view (RT-side) and would introduce a sync. Callers who
	 * need true per-frame LOD should use `stat lodgroup` or trace tools.
	 *
	 * Filters:
	 *   - `ClassFilter`: substring matched against the component's class FName
	 *     (case-insensitive). Typical values: "StaticMeshComponent",
	 *     "SkeletalMeshComponent". Empty = both.
	 *   - `ActorFilter`: substring matched against the owning actor's FName.
	 *     Empty = all actors.
	 *
	 * Schema:
	 *   - Key = "<mesh_asset_path>:LOD<n>"
	 *   - Count = number of components reporting that (mesh, lod) pair
	 *   - TotalBytes = 0 (LOD distribution is component-count, not size)
	 *   - SamplePaths = up to 3 owning actor paths
	 *   - LevelName = empty
	 *
	 * Rows sorted by Count descending, ties by Key. Returns at most 1000 rows.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgePerfBreakdownRow> GetLodDistribution(
		const FString& ClassFilter,
		const FString& ActorFilter);

	/**
	 * Top-N actors ranked by dynamic-shadow caster cost estimate. Walks
	 * the editor world on the GameThread, picks every actor with at least
	 * one primitive component whose `bCastDynamicShadow=true`, computes the
	 * actor's full render cost (same logic as `GetActorRenderCost`), and
	 * returns them sorted by EstimatedTriangleCount descending.
	 *
	 * "Cost estimate" here = LOD0 triangle total of the shadow-casting
	 * primitive components on the actor. Cascade / VSM page costs are
	 * not modeled — those require RT state which we deliberately avoid.
	 *
	 * `TopN` clamped to [1, 1000].
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeActorRenderCost> GetShadowCasterBreakdown(int32 TopN = 30);

	/**
	 * Lumen runtime diagnostics. Returns default-zero on UE 5.6 and below;
	 * on 5.7+ returns visualization mode list + Lumen-enabled CVar state.
	 * See FBridgeLumenDiagnostics for what's surfaced and why surface-cache
	 * / probe-count internals stay unimplemented.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeLumenDiagnostics GetLumenDiagnostics();

	/**
	 * Nanite runtime stats. Returns default-zero on UE 5.6 and below;
	 * on 5.7+ pulls capacity getters from `Nanite::GStreamingManager` plus
	 * a GT-side scan for static-mesh components whose StaticMesh has Nanite
	 * data enabled. Per-frame visible-cluster counters are not exposed
	 * publicly and remain unreported.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeNaniteStats GetNaniteStats();

	// ─── M4: UE Trace integration ──────────────────────────────

	/**
	 * Begin a UE Trace file capture (M4-1). Channels is a list of channel
	 * names (e.g. {"cpu","gpu","frame","rdg","loadtime","memalloc",
	 * "gameplay","slate"}); the engine accepts any registered channel name
	 * — call `ListTraceChannels` to enumerate. Empty list falls back to the
	 * engine's "default" preset.
	 *
	 * `OutputDir` resolution:
	 *   - empty → <Project>/Saved/UnrealBridge/Traces/<unix_ts>.utrace
	 *   - directory → that directory + the auto-named file
	 *   - any other path treated as the output file (parent dir created)
	 *
	 * `MaxSizeMb` is advisory; the engine API doesn't directly enforce a
	 * cap — we record it in state for the caller's bookkeeping (and so a
	 * future watchdog can stop the capture). 0 = unlimited; clamped to
	 * [0, 65536].
	 *
	 * On 5.4+: forwards to `FTraceAuxiliary::Start(EConnectionType::File, ...)`.
	 * On 5.3: falls back to `GEngine->Exec(TEXT("Trace.Start File=..."))`.
	 *
	 * Returns `bSuccess=false, Error="already_active"` when a capture is
	 * already in progress (call `StopTraceCapture` first).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeTraceStartResult StartTraceCapture(
		const TArray<FString>& Channels,
		const FString& OutputDir,
		int32 MaxSizeMb = 500);

	/**
	 * Stop the active UE Trace capture (M4-2). Returns the path that was
	 * being written, the OS-reported file size at stop time, and the wall
	 * clock duration since the matching `StartTraceCapture`. Safe to call
	 * when no capture is active — returns `bSuccess=false` with empty
	 * fields. The internal capture state is reset on success so the next
	 * `StartTraceCapture` can begin cleanly.
	 *
	 * On 5.4+: forwards to `FTraceAuxiliary::Stop()`.
	 * On 5.3: runs `GEngine->Exec(TEXT("Trace.Stop"))`.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeTraceStopResult StopTraceCapture();

	/**
	 * Query the trace capture state (M4-3). On 5.4+ also reconciles the
	 * cached state with `FTraceAuxiliary::IsConnected()`/destination string,
	 * so external `Trace.Start` console commands are reflected. The
	 * `Channels` and `StartedAtUtc` fields reflect only captures started
	 * via this library — engine-initiated captures populate `bActive` and
	 * `Path` but leave the others empty.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeTraceState GetTraceState();

	/**
	 * Enumerate every trace channel the engine knows about (M4-4). Each
	 * entry has the channel `Name`, current `bEnabled` state, and a
	 * possibly-empty `Description`. Channel names follow the convention
	 * "<base>" (e.g. "cpu", "frame"); the engine's internal "Channel"
	 * suffix is stripped.
	 *
	 * On 5.4+: reads via `UE::Trace::EnumerateChannels`. On 5.3: returns
	 * an empty array (the public enumeration API isn't there yet).
	 *
	 * Result is sorted alphabetically by Name.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static TArray<FBridgeTraceChannelInfo> ListTraceChannels();

	/**
	 * Parse a previously-captured `.utrace` file (typically the path returned
	 * by `stop_trace_capture`) into a structured summary (M4-5). Wraps
	 * `TraceServices::IAnalysisService::Analyze` (synchronous), then walks
	 * Diagnostics + Frames + TimingProfiler providers.
	 *
	 * Output:
	 *  - Diagnostics: platform / app / project / build version / changelist
	 *  - Frames (Game type): count, total duration, min / avg / max ms
	 *  - Hot scopes: top-N timers ranked by total inclusive time across all
	 *    CPU thread timelines, with name + total ms + call count
	 *
	 * Cost: dominated by Analyze() — typically 1-10s per 100 MB of trace.
	 * Synchronous, blocks the bridge exec; for large traces, kick this from
	 * an off-main thread or simply tolerate the wait.
	 *
	 * Editor-only path is fine; this works in both editor and cmdlet.
	 *
	 * @param UtracePath       Absolute path to a `.utrace` file. File must exist.
	 * @param TopN             Cap on global CPU + GPU hot-scope rows + load-time
	 *                         breakdown rows (1..1000). Clamped.
	 * @param TopNPerThread    Cap on `TopScopes` per thread row (1..200). Clamped.
	 *                         Setting to 0 skips the per-thread aggregation entirely
	 *                         (saves time + memory on large traces).
	 * @param TopNCounters     Cap on `Counters` rows ranked by SampleCount desc
	 *                         (1..2000). Clamped. Set to 0 to skip the counter
	 *                         walk entirely on huge traces.
	 * @return                 FBridgePerfTraceSummary with `bSuccess=true` on
	 *                         success, or `bSuccess=false` + populated `Error`
	 *                         on any failure (file missing, module load fail,
	 *                         unparseable trace).
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfTraceSummary ParseTraceToSummary(
		const FString& UtracePath,
		int32 TopN = 20,
		int32 TopNPerThread = 10,
		int32 TopNCounters = 100);

	/**
	 * Parse a `.utrace` file's allocation provider into a structured summary
	 * (M6-1). Trace must contain the `memalloc` channel; without it the
	 * AllocationsProvider exists but has no events and the call returns
	 * `bHasEvents=false` with all aggregates at 0.
	 *
	 * Aggregates: peak total allocated memory + peak live allocations (from
	 * the alloc-provider timelines), total alloc / free event counts, alloc
	 * minus free delta, and the full LLM tag inventory.
	 *
	 * Per-allocation top-N "unfreed by size + callstack" requires the alloc
	 * provider's async StartQuery / PollQuery machinery and is deferred to
	 * a later commit. This MVP is enough to detect "did the run leak", peak
	 * memory commit, and which subsystem tags are registered.
	 *
	 * Cost: dominated by Analyze() — alloc traces are large (1 GB activity
	 * = several GB trace). Cap on trace file size enforced by caller's disk
	 * budget; Analyze is synchronous and blocks the bridge exec.
	 *
	 * @param UtracePath  Absolute path to a `.utrace` file. Must exist.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfAllocSummary ParseAllocTraceToSummary(const FString& UtracePath);

	/**
	 * Parse a `.utrace` file's network profiler data into a structured summary
	 * (M6-2). Trace must contain the `net` channel; without it the
	 * NetProfilerProvider exists but reports `NetTraceVersion=0` and the call
	 * returns `bHasEvents=false` with empty game-instance list.
	 *
	 * Output: per game-instance breakdown (server / client, Iris flag, object
	 * count, list of connections) + per-connection packet + byte totals split
	 * by direction (incoming / outgoing). Bytes come from
	 * `FNetProfilerPacket::TotalPacketSizeInBytes` summed across every packet.
	 *
	 * Per-actor replication breakdown + most-expensive-RPC ranking is deferred
	 * (would require walking every packet content event and resolving each
	 * `ObjectInstanceIndex` against the FNetProfilerObjectInstance table —
	 * larger commit). The current MVP is enough to attribute "client X using
	 * Y MB/s" and detect runaway connections.
	 *
	 * @param UtracePath  Absolute path to a `.utrace` file. Must exist.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfNetSummary ParseNetTraceToSummary(const FString& UtracePath);

	/**
	 * Top-N streaming textures by resident GPU bytes (M7-1). Walks every
	 * UTexture2D via TObjectIterator on the GameThread; for each streamable
	 * texture reads `GetStreamableResourceState()` for resident / wanted /
	 * max LOD counts, calls `CalcCumulativeLODSize` to convert mip counts to
	 * bytes, and bundles in pool stats from `IRenderAssetStreamingManager`
	 * (pool size, required pool, over-budget, max-ever-required).
	 *
	 * Use this to find textures that aren't fully streamed in
	 * (`resident_mip_count < wanted_mip_count`) when chasing "why is my
	 * texture blurry" or "why is the streamer over budget".
	 *
	 * Cost: 5-50 ms depending on UTexture2D population. GameThread-only.
	 *
	 * @param TopN  Cap on row count (1..1000). Clamped.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeTextureStreamingState GetTextureStreamingResidency(int32 TopN = 30);

	/**
	 * Render-target memory aggregate (M7-2). See `FBridgeRenderTargetMemory`
	 * for scope — this surfaces UTextureRenderTarget* objects via
	 * TObjectIterator; engine-internal RTs (GBuffer, shadow atlas, Lumen
	 * surface cache) live in renderer-private state and are NOT included.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeRenderTargetMemory GetRenderTargetMemory(int32 TopN = 30);

	/**
	 * Per-pass GPU timings via `FRealtimeGPUProfiler::FetchPerfByDescription`
	 * (M7-3). Returns one row per registered GPU stat scope (BasePass /
	 * ShadowDepths / Lumen / Translucency / PostProcess / etc.) with average,
	 * min and max ms over the profiler's rolling 64-frame history.
	 *
	 * Requires `r.GPUStatsEnabled=1` (default in editor/dev builds) and the
	 * legacy GPU profiler path. When the new RHI GPU profiler is enabled
	 * (`RHI_NEW_GPU_PROFILER=1`) the legacy table is empty — falls back to
	 * `bAvailable=false` with a diagnostic message; use Insights with `gpu`
	 * + `rdg` channels in that case.
	 *
	 * Cost: ~1 ms (read-only scan of the profiler history table). GameThread
	 * only. The data is cumulative since editor launch — represents the
	 * profiler's running average, not last-frame.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeGpuPassTimings GetPerPassGpuTimings();

	/**
	 * Analyse every master material in the project and return the top-N by
	 * structural complexity (M7-4). Walks AssetRegistry for UMaterial assets,
	 * loads each one, counts expression nodes by class (TextureSample / Custom /
	 * StaticSwitch / parameter types), reads blend mode / shading model /
	 * domain / two-sided / skeletal-usage flags.
	 *
	 * Sort key is `ComplexityScore = ExpressionCount + 4×TextureSampleCount +
	 * 8×CustomExpressionCount`. This is a cheap heuristic — for true GPU
	 * instruction counts use the Material Editor's stats panel (requires
	 * MaterialEditor module which is intentionally not a bridge dependency).
	 *
	 * Cost: O(N) load + O(expressions) per material. Loads materials that
	 * weren't already in memory — can be slow on large projects (1-30s).
	 *
	 * @param TopN  Cap on row count (1..1000). Clamped.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeAllMaterialsAnalysis AnalyzeAllMaterials(int32 TopN = 30);

	/**
	 * Parse a `.utrace` file's cook profiler data (M6-3). Trace must contain
	 * the `cook` channel — captured by passing `-trace=cook,...` to the
	 * cooker (`UnrealEditor-Cmd.exe -run=Cook ... -trace=cook`).
	 *
	 * Walks `ICookProfilerProvider::CreateAggregation` to get per-package
	 * timing rows: load + save + BeginCacheForCookedPlatformData + IsCached*
	 * (the four cooker phases). Returns top-N by total cook time desc.
	 *
	 * Used to attribute "4-hour cook" — typically dominated by shader
	 * compilation hidden inside `BeginCacheForCookedPlatformData` for
	 * Material-class packages.
	 *
	 * @param UtracePath  Absolute path to a `.utrace` file. Must exist.
	 * @param TopN        Cap on rows (1..1000). Clamped.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfCookSummary ParseCookTraceToSummary(const FString& UtracePath, int32 TopN = 50);

	/**
	 * Diff two `FBridgePerfSnapshot` instances (M8-2). Returns deltas for
	 * every numeric field plus a list of human-readable regression strings
	 * for any metric that changed by ≥ `RegressionThreshold` (e.g. 0.10 =
	 * "flag anything that got 10% worse"). Memory + frame-time regressions
	 * are flagged when they grow; FPS regressions when they shrink.
	 *
	 * Use case: after an asset edit / refactor, capture a snapshot before
	 * and after and call this to get a structured "what got worse" report.
	 *
	 * Cost: < 1 ms — pure arithmetic + string formatting. No engine state
	 * touched.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgePerfSnapshotDelta ComparePerfSnapshots(
		const FBridgePerfSnapshot& Before,
		const FBridgePerfSnapshot& After,
		float RegressionThreshold = 0.10f);

	/**
	 * Launch UnrealInsights.exe pointing at a `.utrace` file (M8-3). Detached
	 * (Insights runs as its own process); the bridge call returns as soon as
	 * `FPlatformProcess::CreateProc` succeeds. Use when `parse_*_trace_to_summary`
	 * isn't enough and the agent wants a human to take over with the visual
	 * timeline.
	 *
	 * Insights binary location: `<EngineRoot>/Engine/Binaries/Win64/UnrealInsights.exe`.
	 * Resolved automatically via `FPaths::EngineDir()`.
	 */
	UFUNCTION(BlueprintCallable, Category = "UnrealBridge|Perf")
	static FBridgeInsightsLaunchResult BeginInsightsForTrace(const FString& UtracePath);
};
