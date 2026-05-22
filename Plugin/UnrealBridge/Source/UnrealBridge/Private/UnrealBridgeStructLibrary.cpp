#include "UnrealBridgeStructLibrary.h"

#include "Misc/EngineVersionComparison.h"
#include "UnrealBridgeTypeParse.h"

// UUserDefinedStruct header was renamed: lived under `Engine/` in 5.3/5.4,
// moved to `StructUtils/` from 5.5 onwards (the legacy path remained as an
// alias through 5.7 but was removed in 5.8). Pick by version.
#if UE_VERSION_OLDER_THAN(5, 5, 0)
	#include "Engine/UserDefinedStruct.h"
#else
	#include "StructUtils/UserDefinedStruct.h"
#endif
#include "Kismet2/StructureEditorUtils.h"
// Brings in the full definition of FStructVariableDescription (forward-declared
// in StructureEditorUtils.h). GetVarDesc / DescToPinType access its fields.
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Factories/Factory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"

namespace BridgeStructImpl
{

static UUserDefinedStruct* LoadStruct(const FString& Path)
{
	if (Path.IsEmpty()) return nullptr;
	UObject* Obj = StaticLoadObject(UUserDefinedStruct::StaticClass(), nullptr, *Path);
	return Cast<UUserDefinedStruct>(Obj);
}

static bool IsPieRunning()
{
	return GEditor && GEditor->PlayWorld != nullptr;
}

static bool SplitAssetPath(const FString& Path, FString& OutPackagePath, FString& OutAssetName)
{
	int32 LastSlash = INDEX_NONE;
	if (!Path.FindLastChar(TEXT('/'), LastSlash) || LastSlash == 0)
	{
		return false;
	}
	OutPackagePath = Path.Left(LastSlash);
	OutAssetName = Path.Mid(LastSlash + 1);
	// Strip trailing ".Name" if user passed a full object path.
	int32 Dot = INDEX_NONE;
	if (OutAssetName.FindChar(TEXT('.'), Dot))
	{
		OutAssetName = OutAssetName.Left(Dot);
	}
	return !OutAssetName.IsEmpty();
}

/** Locate the UClass of the factory that creates UUserDefinedStruct.
 *  Engine has used two C++ names across versions (`UStructureFactory` is
 *  current); try the known UCLASS paths and pick the first that loads. */
static UClass* GetStructureFactoryClass()
{
	static const TCHAR* Candidates[] = {
		TEXT("/Script/UnrealEd.StructureFactory"),
		TEXT("/Script/UnrealEd.UserDefinedStructureFactory"),
	};
	for (const TCHAR* P : Candidates)
	{
		if (UClass* C = LoadObject<UClass>(nullptr, P))
		{
			if (C->IsChildOf(UFactory::StaticClass()) && !C->HasAnyClassFlags(CLASS_Abstract))
			{
				return C;
			}
		}
	}
	return nullptr;
}

/** The struct's editor-side description array — uses the engine's official
 *  accessor so we don't depend on the layout of `UUserDefinedStructEditorData`. */
static TArray<FStructVariableDescription>* GetVarDescsMutable(UUserDefinedStruct* S)
{
	if (!S) return nullptr;
	return FStructureEditorUtils::GetVarDescPtr(S);
}

/** Find a field by friendly name; returns INDEX_NONE if missing. */
static int32 FindVarIndexByName(UUserDefinedStruct* S, const FString& FriendlyName)
{
	const TArray<FStructVariableDescription>* Descs = GetVarDescsMutable(S);
	if (!Descs) return INDEX_NONE;
	for (int32 i = 0; i < Descs->Num(); ++i)
	{
		const FStructVariableDescription& V = (*Descs)[i];
		// FriendlyName is the user-facing name. When unset (engine sometimes
		// leaves it empty for the placeholder field), fall back to VarName.
		const FString Label = V.FriendlyName.IsEmpty() ? V.VarName.ToString() : V.FriendlyName;
		if (Label.Equals(FriendlyName, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

static FGuid FindVarGuidByName(UUserDefinedStruct* S, const FString& FriendlyName)
{
	const int32 Idx = FindVarIndexByName(S, FriendlyName);
	if (Idx == INDEX_NONE) return FGuid();
	const TArray<FStructVariableDescription>* Descs = GetVarDescsMutable(S);
	return (Descs && Descs->IsValidIndex(Idx)) ? (*Descs)[Idx].VarGuid : FGuid();
}

/** Convert a stored variable description back to a pin type for serialization.
 *  `FStructVariableDescription::SubCategoryObject` is a `TSoftObjectPtr<UObject>`
 *  while `FEdGraphPinType::PinSubCategoryObject` is a `TWeakObjectPtr` — `.Get()`
 *  to a raw pointer bridges the two safely (resolves the soft ptr if loaded). */
static FEdGraphPinType DescToPinType(const FStructVariableDescription& V)
{
	FEdGraphPinType P;
	P.PinCategory          = V.Category;
	P.PinSubCategory       = V.SubCategory;
	P.PinSubCategoryObject = V.SubCategoryObject.Get();
	P.PinValueType         = V.PinValueType;
	P.ContainerType        = V.ContainerType;
	return P;
}

/** Map FStructureEditorUtils::EStructureError to a short text label. */
static FString StatusLabel(UUserDefinedStruct* S)
{
	if (!S) return TEXT("Unknown");
	const auto Err = FStructureEditorUtils::IsStructureValid(S);
	switch (Err)
	{
		case FStructureEditorUtils::Ok:            return TEXT("UpToDate");
		case FStructureEditorUtils::Recursion:     return TEXT("Error_Recursion");
		case FStructureEditorUtils::FallbackStruct: return TEXT("Error_FallbackStruct");
		case FStructureEditorUtils::NotCompiled:   return TEXT("NotCompiled");
		case FStructureEditorUtils::NotBlueprintType: return TEXT("Error_NotBlueprintType");
		case FStructureEditorUtils::NotSupportedType: return TEXT("Error_NotSupportedType");
		case FStructureEditorUtils::EmptyStructure: return TEXT("Error_EmptyStructure");
		default:                                   return TEXT("Unknown");
	}
}

} // namespace BridgeStructImpl


// ─── Asset lifecycle ────────────────────────────────────────────────

FBridgeStructCreateResult UUnrealBridgeStructLibrary::CreateUserDefinedStruct(const FString& AssetPath)
{
	using namespace BridgeStructImpl;

	FBridgeStructCreateResult Result;

	if (IsPieRunning())
	{
		Result.Error = TEXT("refusing to create UserDefinedStruct while PIE is running");
		return Result;
	}

	FString PackagePath, AssetName;
	if (!SplitAssetPath(AssetPath, PackagePath, AssetName))
	{
		Result.Error = FString::Printf(TEXT("invalid path '%s' — expected /Game/Folder/AssetName"), *AssetPath);
		return Result;
	}

	UClass* FactoryClass = GetStructureFactoryClass();
	if (!FactoryClass)
	{
		Result.Error = TEXT("could not locate a UUserDefinedStruct factory class — engine may not have UnrealEd loaded");
		return Result;
	}

	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	if (!Factory)
	{
		Result.Error = TEXT("NewObject<UFactory> returned null");
		return Result;
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath,
		UUserDefinedStruct::StaticClass(), Factory);

	UUserDefinedStruct* NewStruct = Cast<UUserDefinedStruct>(NewAsset);
	if (!NewStruct)
	{
		Result.Error = FString::Printf(TEXT("AssetTools.CreateAsset returned null for %s/%s — path may already be occupied"),
			*PackagePath, *AssetName);
		return Result;
	}

	NewStruct->MarkPackageDirty();

	Result.bSuccess = true;
	Result.Path = NewStruct->GetPathName();
	return Result;
}


// ─── Field CRUD ─────────────────────────────────────────────────────

bool UUnrealBridgeStructLibrary::AddStructVariable(const FString& StructPath,
	const FString& Name, const FString& TypeString, const FString& DefaultValue)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	FEdGraphPinType PinType;
	if (!BridgeTypeParseImpl::ParseTypeString(TypeString, PinType)) return false;

	// Snapshot existing GUIDs so we can identify the freshly added field
	// (FStructureEditorUtils::AddVariable doesn't return the new GUID).
	TSet<FGuid> Existing;
	if (const TArray<FStructVariableDescription>* Descs = GetVarDescsMutable(S))
	{
		for (const auto& V : *Descs) { Existing.Add(V.VarGuid); }
	}

	if (!FStructureEditorUtils::AddVariable(S, PinType)) return false;

	FGuid NewGuid;
	if (const TArray<FStructVariableDescription>* Descs = GetVarDescsMutable(S))
	{
		for (const auto& V : *Descs)
		{
			if (!Existing.Contains(V.VarGuid)) { NewGuid = V.VarGuid; break; }
		}
	}
	if (!NewGuid.IsValid()) return false;

	if (!Name.IsEmpty())
	{
		if (!FStructureEditorUtils::RenameVariable(S, NewGuid, Name)) return false;
	}

	if (!DefaultValue.IsEmpty())
	{
		// ChangeVariableDefaultValue accepts a serialized FProperty value.
		FStructureEditorUtils::ChangeVariableDefaultValue(S, NewGuid, DefaultValue);
	}

	return true;
}

bool UUnrealBridgeStructLibrary::RemoveStructVariable(const FString& StructPath, const FString& Name)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	const FGuid Guid = FindVarGuidByName(S, Name);
	if (!Guid.IsValid()) return false;

	return FStructureEditorUtils::RemoveVariable(S, Guid);
}

bool UUnrealBridgeStructLibrary::RenameStructVariable(const FString& StructPath,
	const FString& OldName, const FString& NewName)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;
	if (NewName.IsEmpty()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	const FGuid Guid = FindVarGuidByName(S, OldName);
	if (!Guid.IsValid()) return false;

	return FStructureEditorUtils::RenameVariable(S, Guid, NewName);
}

bool UUnrealBridgeStructLibrary::ChangeStructVariableType(const FString& StructPath,
	const FString& Name, const FString& NewTypeString)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	FEdGraphPinType NewType;
	if (!BridgeTypeParseImpl::ParseTypeString(NewTypeString, NewType)) return false;

	const FGuid Guid = FindVarGuidByName(S, Name);
	if (!Guid.IsValid()) return false;

	return FStructureEditorUtils::ChangeVariableType(S, Guid, NewType);
}

bool UUnrealBridgeStructLibrary::SetStructVariableDefault(const FString& StructPath,
	const FString& Name, const FString& DefaultValue)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	const FGuid Guid = FindVarGuidByName(S, Name);
	if (!Guid.IsValid()) return false;

	return FStructureEditorUtils::ChangeVariableDefaultValue(S, Guid, DefaultValue);
}

bool UUnrealBridgeStructLibrary::MoveStructVariable(const FString& StructPath,
	const FString& Name, int32 NewIndex)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	const int32 CurrentIdx = FindVarIndexByName(S, Name);
	if (CurrentIdx == INDEX_NONE) return false;

	const TArray<FStructVariableDescription>* Descs = GetVarDescsMutable(S);
	if (!Descs || Descs->Num() == 0) return false;

	NewIndex = FMath::Clamp(NewIndex, 0, Descs->Num() - 1);
	if (NewIndex == CurrentIdx) return true;

	const FGuid MoveGuid = (*Descs)[CurrentIdx].VarGuid;

	// FStructureEditorUtils::MoveVariable is a single relative-to-neighbor op:
	// "place this variable immediately above or below a reference variable".
	// To translate "move to index NewIndex", we find what the neighbor at
	// NewIndex is in the *other* variables' ordering (excluding the moving
	// var itself) and ask to be placed above or below it.
	TArray<FGuid> OtherGuids;
	OtherGuids.Reserve(Descs->Num() - 1);
	for (const FStructVariableDescription& V : *Descs)
	{
		if (V.VarGuid != MoveGuid) { OtherGuids.Add(V.VarGuid); }
	}
	if (OtherGuids.Num() == 0) return true;  // single-field move is a no-op

	FGuid RelativeGuid;
	FStructureEditorUtils::EMovePosition Position;
	if (NewIndex <= 0)
	{
		RelativeGuid = OtherGuids[0];
		Position = FStructureEditorUtils::PositionAbove;
	}
	else if (NewIndex >= OtherGuids.Num())
	{
		RelativeGuid = OtherGuids.Last();
		Position = FStructureEditorUtils::PositionBelow;
	}
	else
	{
		RelativeGuid = OtherGuids[NewIndex];
		Position = FStructureEditorUtils::PositionAbove;
	}
	return FStructureEditorUtils::MoveVariable(S, MoveGuid, RelativeGuid, Position);
}


// ─── Reads ──────────────────────────────────────────────────────────

TArray<FBridgeStructVariableInfo>
UUnrealBridgeStructLibrary::GetStructVariables(const FString& StructPath)
{
	using namespace BridgeStructImpl;

	TArray<FBridgeStructVariableInfo> Out;
	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return Out;

	const TArray<FStructVariableDescription>* Descs = GetVarDescsMutable(S);
	if (!Descs) return Out;

	for (const FStructVariableDescription& V : *Descs)
	{
		FBridgeStructVariableInfo Info;
		Info.Name = V.FriendlyName.IsEmpty() ? V.VarName.ToString() : V.FriendlyName;
		Info.TypeString = BridgeTypeParseImpl::PinTypeToString(DescToPinType(V));
		Info.DefaultValue = V.DefaultValue;
		Info.Guid = V.VarGuid.ToString();
		Info.Tooltip = V.ToolTip;
		Info.bEditOnInstance = !V.bDontEditOnInstance;
		Out.Add(Info);
	}
	return Out;
}

FBridgeStructInfo UUnrealBridgeStructLibrary::GetStructInfo(const FString& StructPath)
{
	using namespace BridgeStructImpl;

	FBridgeStructInfo Info;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return Info;

	Info.Path = S->GetPathName();
	Info.Status = StatusLabel(S);
	Info.Tooltip = FStructureEditorUtils::GetTooltip(S);
	Info.VariableCount = FStructureEditorUtils::GetVarDesc(S).Num();

	return Info;
}


// ─── Metadata ───────────────────────────────────────────────────────

bool UUnrealBridgeStructLibrary::SetStructVariableTooltip(const FString& StructPath,
	const FString& Name, const FString& Tooltip)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	const FGuid Guid = FindVarGuidByName(S, Name);
	if (!Guid.IsValid()) return false;

	return FStructureEditorUtils::ChangeVariableTooltip(S, Guid, Tooltip);
}

bool UUnrealBridgeStructLibrary::SetStructVariableEditOnInstance(const FString& StructPath,
	const FString& Name, bool bEditOnInstance)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	const FGuid Guid = FindVarGuidByName(S, Name);
	if (!Guid.IsValid()) return false;

	return FStructureEditorUtils::ChangeEditableOnBPInstance(S, Guid, bEditOnInstance);
}

bool UUnrealBridgeStructLibrary::SetStructTooltip(const FString& StructPath, const FString& Tooltip)
{
	using namespace BridgeStructImpl;

	if (IsPieRunning()) return false;

	UUserDefinedStruct* S = LoadStruct(StructPath);
	if (!S) return false;

	return FStructureEditorUtils::ChangeTooltip(S, Tooltip);
}
