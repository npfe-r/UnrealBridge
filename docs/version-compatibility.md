# UnrealBridge — engine version compatibility

The plugin claims **Unreal Engine 5.3+**. The build matrix in
`tools/build_matrix.py` has verified clean BuildPlugin against 5.3 / 5.4 /
5.5 / 5.6 / 5.7 / 5.8 with the gating below; 5.2 is configured but disabled
(would need additional pre-5.3 shims). Some features are gated to **5.7+**
because they depend on engine APIs that don't exist or behave differently
on the older 5.x's. A handful of **5.8-only** shims handle API renames /
parameter additions that landed in 5.8.

This document lists what's gated. The build matrix in `tools/build_matrix.py`
verifies the gates by compiling the plugin against each engine version.

## Whole-library gates (disappear entirely on 5.4)

Each library below is wrapped in `#if !UE_VERSION_OLDER_THAN(5, 7, 0)`. On 5.4
its `.h` and `.cpp` compile to empty translation units, no `UCLASS` is
registered, and calling any of its UFUNCTIONs from Python on a 5.4 build will
fail with "no such function on UnrealBridgeXxxLibrary".

| Library | Reason |
|---|---|
| `UnrealBridgeChooserLibrary` | `OutputObjectColumn.h` doesn't exist in 5.4 (added with the Chooser plugin's output-column rewrite); other Chooser internals shifted heavily 5.4 → 5.7 |
| `UnrealBridgePoseSearchLibrary` | Core API rewritten: `UPoseSearchSchema::GetRoledSkeletons`, `UPoseSearchDatabase::GetNumAnimationAssets` / `GetDatabaseAnimationAsset`, and `FPoseSearchDatabaseAnimationAsset` are all 5.5+ additions |
| `UnrealBridgeMaterialLibrary` | `EMaterialDomain::MD_*` enum values differ in scope; `MATUSAGE_Voxels` / `MATUSAGE_StaticMesh` don't exist in 5.4 |
| `UnrealBridgeNavigationLibrary` | `ARecastNavMesh::GetDebugGeometryForTile` 2nd arg type changed (`int32` → `FNavTileRef`) and the "default tile = aggregate all" sentinel doesn't exist on 5.4 |

## Single-UFUNCTION gates (library still works, one function unavailable on 5.4)

| UFUNCTION | Reason |
|---|---|
| `UnrealBridgeDataTableLibrary::CopyDataTableRows` | `UDataTable::AddRow(FName, const uint8*, UScriptStruct*)` 3-arg overload is 5.7+ |
| `UnrealBridgeBlueprintLibrary::AddAsyncActionNode` | `UK2Node_AsyncAction::InitializeProxyFromFunction` doesn't exist on 5.4 |
| `UnrealBridgeGameplayAbilityLibrary::AddAbilityTaskNode` | `UK2Node_LatentAbilityCall` UCLASS in `GameplayAbilitiesEditor` is non-API in 5.4; `NewObject<>` of it fails to link from external modules |

## Inline shims (function works on both, different code paths)

These remain callable on 5.4 — the macro picks the right code path internally.

| Function | What 5.4 lacks | Shim |
|---|---|---|
| `UnrealBridgeAnimLibrary::SetAnimStateDefault` | `UAnimStateEntryNode::GetOutputPin()` | walk `Entry->Pins[]` for the first `EGPD_Output` pin |
| `UnrealBridgeGameplayAbilityLibrary::GetGameplayAbilityBlueprintInfo` and `ListGameplayAbilitiesByTag` | `UGameplayAbility::GetAssetTags()` | read legacy `CDO->AbilityTags` field |
| `UnrealBridgeBlueprintLibrary::GetPIENodeCoverage` | `FKismetDebugUtilities::FindSourceNodeForCodeLocation` const-correctness | `const_cast<UFunction*>(Func)` |
| `UnrealBridgeGameplayTagLibrary` — `EnsureSourceRedirectsPersisted`, `RenameGameplayTag`, `RemoveGameplayTagRedirect`, `ListGameplayTagRedirects` | 5.5 lacks `UGameplayTagsList::GameplayTagRedirects` (on `UGameplayTagsSettings` only); `RenameTagInINI` 3-arg overload added in 5.7 | `!UE_VERSION_OLDER_THAN(5, 6, 0)`: use `SourceTagList->GameplayTagRedirects`; legacy: parse per-source `+GameplayTagRedirects=` lines from disk ini. `RenameTagInINI` gated at `!UE_VERSION_OLDER_THAN(5, 7, 0)` for the `bRenameChildren` parameter |
| `UnrealBridgeAnimLibrary` — `GetAnimGraphNodes`, `ListAnimSlotsInABP`, `DumpAnimNodeProperties` | `UAnimGraphNode_Base::GetFNode` / `GetFNodeProperty` / `GetFNodeType` were `protected` before 5.4 (made `public` in 5.4) | `BridgeAnimNodeAccess::*` shim — gated at `UE_VERSION_OLDER_THAN(5, 4, 0)`, exposes the protected getters via `using`-declaration in a derived helper struct (compile-time access bypass, never instantiated) |
| All `TArray::Pop` / `RemoveAt` call sites with explicit shrinking flag | `EAllowShrinking` enum added in 5.4 (replaced `bool bAllowShrinking`) | `Plugin/.../Private/UnrealBridgeCompat.h` — defines `namespace EAllowShrinking { static constexpr bool No, Yes; }` on pre-5.4 so `Array.Pop(EAllowShrinking::No)` resolves to the legacy `bool` overload |
| `UnrealBridgeBlueprintLibrary` — Blueprint exception/debug paths | `Blueprint/BlueprintExceptionInfo.h` is 5.4+ only (split out of `UObject/Script.h`) | `#if !UE_VERSION_OLDER_THAN(5, 4, 0)` around the include; on 5.3 `FBlueprintExceptionInfo` is reachable via the already-included `UObject/Script.h` |
| `UnrealBridgeGameplayAbilityLibrary::ScanProperty` map/set iteration | 5.4 added `FScriptMapHelper::GetKeyPtr(FIterator)` / `GetValuePtr(FIterator)` / `FScriptSetHelper::GetElementPtr(FIterator)` overloads; 5.3 only has the `int32` overloads; 5.8 removed the deprecated `FIterator::operator*() → int32` so `*It` no longer compiles there | Per-call gate `#if UE_VERSION_OLDER_THAN(5, 4, 0)`: pass `*It` to the int32 overload on 5.3; on 5.4+ pass `It` directly to the FIterator overload (works through 5.8) |
| `UnrealBridgeBlueprintLibrary::ExecuteBlueprintFunction` and `UnrealBridgeLevelLibrary::ExecuteActorFunction` JSON args | 5.8 changed `FJsonObjectConverter::JsonAttributesToUStruct`'s first parameter from `TMap<FString, ...>` to `TMap<UE::FSharedString, ...>` (also `FJsonObject::Values`) | `Private/UnrealBridgeCompat.h` defines `FBridgeJsonAttrsKey` (`FString` on <5.8, `UE::FSharedString` on 5.8+); call sites build the map with that alias and `.Add(FBridgeJsonAttrsKey(*Prop->GetName()), Val)` works on every version (both `FString` and `UE::FSharedString` have a `const TCHAR*` ctor) |
| `UnrealBridgeReactiveSubsystem` JSON-object key copy | Same `FJsonObject::Values` 5.8 widening as above — `Pair.Key` is `UE::FSharedString` on 5.8 and won't convert implicitly to `FString` map key | Use `FString(*Pair.Key)` at the insertion site — `operator*` on both string types returns `const TCHAR*`, so the construction works on every version with no macro |
| `UnrealBridgeChooserLibrary::DeleteChooserRow` | 5.8 changed `FChooserColumnBase::DeleteRows` from `const TArray<uint32>&` to `TArrayView<int>` | `#if !UE_VERSION_OLDER_THAN(5, 8, 0)`: build a stack `int[]` and pass `MakeArrayView`; legacy: keep the `TArray<uint32>` form |
| `UnrealBridgeChooserLibrary::EvaluateChooser` debug-row readback | 5.8 renamed `UChooserTable::GetDebugSelectedRow() → int32` to `GetDebugSelectedRows() → const TArray<int32>&` (multi-row support) | `#if !UE_VERSION_OLDER_THAN(5, 8, 0)`: read `GetDebugSelectedRows()[0]` if non-empty, else `-1`; legacy: keep the singular `GetDebugSelectedRow()` |
| `UnrealBridgeGeometryLibrary::DisplaceMeshFromTexture` | 5.8 inserted a new `FGeometryScriptAdaptiveTessellationOptions` parameter (position 5) into `UGeometryScriptLibrary_MeshDeformFunctions::ApplyDisplaceFromTextureMap` | `#if !UE_VERSION_OLDER_THAN(5, 8, 0)`: pass a default-constructed `FGeometryScriptAdaptiveTessellationOptions{}` between `Options` and `UVChannel`; legacy: omit the parameter |

## How the gate macro works

```cpp
#include "Misc/EngineVersionComparison.h"

#if !UE_VERSION_OLDER_THAN(5, 7, 0)
// only compiled on 5.7+
#endif
```

The 5.7 threshold for the whole-library and single-UFUNCTION gates above
is conservative — those APIs may work on 5.5 / 5.6 too, but until the
matrix proves a lowered threshold passes BuildPlugin (the gated-IN code
isn't compiled on 5.5 / 5.6 yet, only the gated-OUT empty stubs are), we
keep the cutoff at 5.7. Inline shims, by contrast, *are* compiled on
every version in the matrix — so each `UE_VERSION_OLDER_THAN(M, m, 0)`
gate listed in the table above is verified across all Last verified
versions below.

## Verifying

```
python tools/build_matrix.py             # build against all configured engines
python tools/build_matrix.py --only 5.4  # only 5.4
python tools/build_matrix.py --only 5.7  # only 5.7
```

Last verified versions:
- UE 5.3 (point release: 5.3.2)
- UE 5.4 (point release: 5.4.4)
- UE 5.5 (point release: 5.5.4)
- UE 5.6 (point release: 5.6.1)
- UE 5.7 (point release: 5.7.1)
- UE 5.8 (point release: 5.8.0, source build) — requires `UnrealBuildTool_BuildConfiguration__bAllowUBAExecutor=false` env var (configured per-engine via `tools/engines.local.json`'s `env` block) on engine snapshots that lack the `UbaDetours.dll` content fetched by Setup.bat. UAT detours-injects `UbaDetours.dll` into cl.exe; the env var disables UBA and falls back to the local ParallelExecutor

## Toolchain note: 5.3 / 5.4 vs. MSVC ≥ 14.44

5.3 / 5.4 engine source contains an unguarded `#elif __has_feature(...)` in
`Engine/Source/Runtime/Core/Public/Experimental/ConcurrentLinearAllocator.h`
(line 31). MSVC 14.44+ rejects it with `C4668: '__has_feature' is not
defined as a preprocessor macro` followed by `C4067`. This is an
**engine-side** issue (fixed in 5.5+ which guards with
`#elif defined(__has_feature) / #if __has_feature(...)`), unrelated to
the plugin.

UnrealBridge code itself compiles cleanly against 5.3 / 5.4 — the
break is in `SharedPCH.UnrealEd.Cpp20.cpp`, an engine TU. Plugin
`Build.cs` settings don't reach into engine PCH compilation, and UBT
5.3 / 5.4 don't expose a user-configurable `AdditionalArguments` knob
for cl.exe flags.

**The project does not patch the engine.** If you're contributing on a
machine with MSVC ≥ 14.44 and need to verify 5.3 / 5.4 locally, you
have two options — both **outside** the repo:

1. Install an older MSVC (≤ 14.40, i.e. VS 2022 17.10) side-by-side
   via the Visual Studio Installer; pin UE 5.3 / 5.4 to it via
   `WindowsPlatform.CompilerVersion` in
   `%APPDATA%/Unreal Engine/UnrealBuildTool/BuildConfiguration.xml`.
2. Apply the 5.7 fix to your local engine install manually (one file,
   one `#elif` replacement). **Don't commit this to UnrealBridge**, and
   re-apply if the Launcher re-verifies.

If neither option is convenient, disable 5.3 / 5.4 in your local
`tools/engines.local.json` (gitignored) so build_matrix skips them —
plugin code is still version-compatible, it just can't be verified
against those engines on your toolchain.

**5.2 is known-broken and unsupported.** The matrix was exercised against
it once (2026-05-06) and surfaced API drifts in PerfLibrary
(`RHIGlobals.h` is 5.3+), CurveLibrary (`RCTM_SmartAuto` enum value,
`FFloatCurve::GetName`), AnimLibrary (`USkeleton::GetCurveMetaDataNames`),
AssetLibrary (`UStaticMesh::IsNaniteEnabled`), GameplayAbilityLibrary
(`GameplayEffectComponent.h` is 5.4+), spread across 6 libraries — and
the build aborted at the first failing module, so the full extent is
larger. Supporting 5.2 would require either substantial inline shims or
whole-library 5.3+ gates. Given UE 5.2 was released 2023-05 and is no
longer in Epic's launcher, the project does not support it.

## Lowering the threshold

When you install another 5.x and add it to `tools/engines.local.json`,
re-running the matrix will reveal which of the 5.7-gated items also work on
the new version. To lower a gate, e.g. from 5.7 to 5.5:

1. Replace `UE_VERSION_OLDER_THAN(5, 7, 0)` with `UE_VERSION_OLDER_THAN(5, 5, 0)`
   on the affected sites (in both the main `.cpp` body gate and the
   corresponding `_Stubs.cpp` inverse gate).
2. Re-run the matrix; verify the lowered version passes.
3. Update the tables above.
