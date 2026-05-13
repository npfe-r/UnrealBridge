// UnrealBridgeCompat.h — cross-version compatibility shims for the bridge plugin.
// Centralised so individual .cpp files don't sprout local #if blocks per call site.
#pragma once

#include "Misc/EngineVersionComparison.h"

// ─── EAllowShrinking shim (pre-5.4) ─────────────────────────
// Added in 5.4 as the standard way to suppress TArray reallocation on Pop/RemoveAt.
// Pre-5.4 these methods take `bool bAllowShrinking` instead. On 5.3 we expose a
// namespace named EAllowShrinking with bool constants so call sites like
// `Array.Pop(EAllowShrinking::No)` compile uniformly across versions.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
namespace EAllowShrinking
{
	static constexpr bool No  = false;
	static constexpr bool Yes = true;
}
#endif

// ─── JsonAttributesToUStruct key type (5.8) ─────────────────
// 5.8 changed the first parameter of FJsonObjectConverter::JsonAttributesToUStruct
// from `TMap<FString, TSharedPtr<FJsonValue>>` to
// `TMap<UE::FSharedString, TSharedPtr<FJsonValue>>`. Call sites use this alias as
// the local map key type so the same code compiles on every supported engine.
// UE::FSharedString has a `const TCHAR*` ctor — wrap FString keys as
// `FBridgeJsonAttrsKey(*StringValue)` when inserting.
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
#include "Containers/SharedString.h"
using FBridgeJsonAttrsKey = UE::FSharedString;
#else
using FBridgeJsonAttrsKey = FString;
#endif
