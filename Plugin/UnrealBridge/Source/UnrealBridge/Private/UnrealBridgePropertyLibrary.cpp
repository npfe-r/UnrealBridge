#include "UnrealBridgePropertyLibrary.h"

#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "UnrealBridgePropertyLibrary"

namespace BridgePropertyOps
{
	// ── Object resolution ─────────────────────────────────────────

	/** Accept asset path / explicit object path / class path; return
	 *  candidates in priority order: prefer the runtime CDO (Modifiers /
	 *  GE fields live there), but keep the UBlueprint asset as a fallback
	 *  so caller can also read asset-level fields like ParentClass /
	 *  SimpleConstructionScript. */
	TArray<UObject*> ResolveCandidates(const FString& Path)
	{
		TArray<UObject*> Candidates;
		if (Path.IsEmpty()) return Candidates;

		UObject* Obj = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
		if (!Obj)
		{
			if (UClass* Cls = StaticLoadClass(UObject::StaticClass(), nullptr, *Path))
			{
				Candidates.Add(Cls->GetDefaultObject());
			}
			return Candidates;
		}

		if (UBlueprint* BP = Cast<UBlueprint>(Obj))
		{
			if (BP->GeneratedClass)
			{
				Candidates.Add(BP->GeneratedClass->GetDefaultObject());  // CDO first (Modifiers etc.)
			}
			Candidates.Add(BP);  // asset itself second (ParentClass etc.)
			return Candidates;
		}

		if (UClass* Cls = Cast<UClass>(Obj))
		{
			Candidates.Add(Cls->GetDefaultObject());
			return Candidates;
		}

		Candidates.Add(Obj);
		return Candidates;
	}

	/** Single-result legacy resolver — kept for callers that don't need
	 *  fallback (e.g. CDO path helper). */
	UObject* ResolveObjectOrClass(const FString& Path)
	{
		const TArray<UObject*> Cands = ResolveCandidates(Path);
		return Cands.Num() > 0 ? Cands[0] : nullptr;
	}

	// ── Path parsing ──────────────────────────────────────────────

	struct FPathSegment
	{
		FString Name;
		int32 Index = INDEX_NONE;  // INDEX_NONE = no [N] suffix
	};

	TArray<FPathSegment> ParsePath(const FString& Path)
	{
		TArray<FPathSegment> Out;
		if (Path.IsEmpty()) return Out;

		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty=*/true);
		for (const FString& Part : Parts)
		{
			FPathSegment Seg;
			int32 OpenIdx, CloseIdx;
			if (Part.FindChar(TEXT('['), OpenIdx) && Part.FindChar(TEXT(']'), CloseIdx) && CloseIdx > OpenIdx + 1)
			{
				Seg.Name = Part.Left(OpenIdx);
				const FString IdxStr = Part.Mid(OpenIdx + 1, CloseIdx - OpenIdx - 1);
				Seg.Index = FCString::Atoi(*IdxStr);
			}
			else
			{
				Seg.Name = Part;
			}
			Out.Add(MoveTemp(Seg));
		}
		return Out;
	}

	// ── Property walker ───────────────────────────────────────────

	struct FResolvedProp
	{
		UObject* RootObject = nullptr;
		FProperty* LeafProperty = nullptr;
		void* LeafValuePtr = nullptr;
		TArray<FProperty*> PropertyChain;  // root → leaf, for PostEditChangeChain
	};

	FResolvedProp ResolvePath(UObject* Root, const TArray<FPathSegment>& Path)
	{
		FResolvedProp Out;
		Out.RootObject = Root;
		if (!Root || Path.Num() == 0) return Out;

		void* CurrentContainerPtr = Root;
		UStruct* CurrentStruct = Root->GetClass();
		FProperty* CurrentProp = nullptr;
		void* CurrentValuePtr = nullptr;

		for (int32 i = 0; i < Path.Num(); ++i)
		{
			const FPathSegment& Seg = Path[i];

			FProperty* Prop = CurrentStruct ? CurrentStruct->FindPropertyByName(FName(*Seg.Name)) : nullptr;
			if (!Prop)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("BridgeProperty: segment '%s' not found in %s"),
					*Seg.Name, CurrentStruct ? *CurrentStruct->GetName() : TEXT("<null>"));
				return FResolvedProp();
			}

			Out.PropertyChain.Add(Prop);
			CurrentProp = Prop;
			CurrentValuePtr = Prop->ContainerPtrToValuePtr<void>(CurrentContainerPtr);

			// [N] indexing
			if (Seg.Index != INDEX_NONE)
			{
				FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
				if (!ArrayProp)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("BridgeProperty: '[%d]' on non-array property '%s'"),
						Seg.Index, *Seg.Name);
					return FResolvedProp();
				}
				FScriptArrayHelper Helper(ArrayProp, CurrentValuePtr);
				int32 RealIdx = Seg.Index < 0 ? Helper.Num() + Seg.Index : Seg.Index;
				if (RealIdx < 0 || RealIdx >= Helper.Num())
				{
					UE_LOG(LogTemp, Warning,
						TEXT("BridgeProperty: index %d out of range [0,%d) for '%s'"),
						Seg.Index, Helper.Num(), *Seg.Name);
					return FResolvedProp();
				}
				CurrentValuePtr = Helper.GetRawPtr(RealIdx);
				CurrentProp = ArrayProp->Inner;  // continue traversal as the element type
			}

			// If not the last segment, recurse into struct
			if (i < Path.Num() - 1)
			{
				FStructProperty* StructProp = CastField<FStructProperty>(CurrentProp);
				if (!StructProp)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("BridgeProperty: cannot recurse past non-struct property '%s'"),
						*Seg.Name);
					return FResolvedProp();
				}
				CurrentContainerPtr = CurrentValuePtr;
				CurrentStruct = StructProp->Struct;
			}
		}

		Out.LeafProperty = CurrentProp;
		Out.LeafValuePtr = CurrentValuePtr;
		return Out;
	}

	// ── Change-notify helper ──────────────────────────────────────

	void FireChangeNotify(UObject* Obj, const TArray<FProperty*>& Chain)
	{
		if (!Obj || Chain.Num() == 0) return;

		FEditPropertyChain PropChain;
		for (FProperty* P : Chain) PropChain.AddTail(P);
		PropChain.SetActivePropertyNode(Chain.Last());
		PropChain.SetActiveMemberPropertyNode(Chain[0]);

		FPropertyChangedEvent EditEvent(Chain.Last(), EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent ChainEvent(PropChain, EditEvent);
		Obj->PostEditChangeChainProperty(ChainEvent);
	}

	// ── Flag decoders ─────────────────────────────────────────────

	FString DecodeAccess(EPropertyFlags Flags)
	{
		if (Flags & CPF_NativeAccessSpecifierPublic)    return TEXT("Public");
		if (Flags & CPF_NativeAccessSpecifierProtected) return TEXT("Protected");
		if (Flags & CPF_NativeAccessSpecifierPrivate)   return TEXT("Private");
		return TEXT("Unknown");  // bare UPROPERTY() with no native access marker
	}

	TArray<FString> DecodeFlags(EPropertyFlags Flags)
	{
		TArray<FString> Out;

		// Edit visibility — combine CPF_Edit with the two disable flags
		const bool bEdit       = !!(Flags & CPF_Edit);
		const bool bNoInstance = !!(Flags & CPF_DisableEditOnInstance);
		const bool bNoTemplate = !!(Flags & CPF_DisableEditOnTemplate);
		if (bEdit)
		{
			if (bNoInstance && !bNoTemplate)      Out.Add(TEXT("EditDefaultsOnly"));
			else if (!bNoInstance && bNoTemplate) Out.Add(TEXT("EditInstanceOnly"));
			else                                   Out.Add(TEXT("EditAnywhere"));
		}

		// Blueprint visibility
		if (Flags & CPF_BlueprintVisible)
		{
			Out.Add((Flags & CPF_BlueprintReadOnly) ? TEXT("BlueprintReadOnly") : TEXT("BlueprintReadWrite"));
		}

		// Other commonly-needed flags
		if (Flags & CPF_Transient)        Out.Add(TEXT("Transient"));
		if (Flags & CPF_Config)           Out.Add(TEXT("Config"));
		if (Flags & CPF_Net)              Out.Add(TEXT("Replicated"));
		if (Flags & CPF_RepNotify)        Out.Add(TEXT("RepNotify"));
		if (Flags & CPF_Interp)           Out.Add(TEXT("Interp"));
		if (Flags & CPF_NoClear)          Out.Add(TEXT("NoClear"));
		if (Flags & CPF_EditFixedSize)    Out.Add(TEXT("EditFixedSize"));
		if (Flags & CPF_AdvancedDisplay)  Out.Add(TEXT("AdvancedDisplay"));

		return Out;
	}
}  // namespace BridgePropertyOps

// ═══════════════════════════════════════════════════════════════
//  Public UFUNCTIONs
// ═══════════════════════════════════════════════════════════════

TArray<FBridgeUPropertyInfo> UUnrealBridgePropertyLibrary::ListUProperties(
	const FString& ObjectOrClassPath, bool bIncludeInherited)
{
	TArray<FBridgeUPropertyInfo> Result;
	UObject* Obj = BridgePropertyOps::ResolveObjectOrClass(ObjectOrClassPath);
	if (!Obj) return Result;

	UClass* Cls = Obj->GetClass();
	if (!Cls) return Result;

	for (TFieldIterator<FProperty> It(Cls,
			bIncludeInherited ? EFieldIteratorFlags::IncludeSuper : EFieldIteratorFlags::ExcludeSuper);
		It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;

		FBridgeUPropertyInfo Info;
		Info.Name = Prop->GetName();
		Info.TypeName = Prop->GetCPPType();
		if (UClass* Owner = Prop->GetOwnerClass())
		{
			Info.DeclaringClassPath = Owner->GetPathName();
		}
		const EPropertyFlags PropFlags = Prop->GetPropertyFlags();
		Info.CppAccess = BridgePropertyOps::DecodeAccess(PropFlags);
		Info.Flags = BridgePropertyOps::DecodeFlags(PropFlags);

		// Metadata
		const TMap<FName, FString>* MetaMap = Prop->GetMetaDataMap();
		if (MetaMap)
		{
			for (const auto& Pair : *MetaMap)
			{
				Info.Metadata.Add(Pair.Key.ToString(), Pair.Value);
			}
		}

		Info.bIsContainer = (CastField<FArrayProperty>(Prop) != nullptr)
		                 || (CastField<FMapProperty>(Prop)   != nullptr)
		                 || (CastField<FSetProperty>(Prop)   != nullptr);
		Info.bIsStructValue = (CastField<FStructProperty>(Prop) != nullptr);

		Result.Add(MoveTemp(Info));
	}

	return Result;
}

FString UUnrealBridgePropertyLibrary::GetUPropertyAsExportText(
	const FString& ObjectOrClassPath, const FString& PropertyPath, bool& OutSuccess)
{
	OutSuccess = false;

	const TArray<FName> SegName_Unused;  // unused, just to silence
	const auto Path = BridgePropertyOps::ParsePath(PropertyPath);
	const TArray<UObject*> Candidates = BridgePropertyOps::ResolveCandidates(ObjectOrClassPath);

	for (UObject* Obj : Candidates)
	{
		const auto Resolved = BridgePropertyOps::ResolvePath(Obj, Path);
		if (!Resolved.LeafProperty || !Resolved.LeafValuePtr) continue;

		FString Out;
		Resolved.LeafProperty->ExportTextItem_Direct(
			Out, Resolved.LeafValuePtr, /*DefaultValue=*/nullptr, Obj, PPF_None);
		OutSuccess = true;
		return Out;
	}
	return FString();
}

bool UUnrealBridgePropertyLibrary::SetUPropertyFromExportText(
	const FString& ObjectOrClassPath, const FString& PropertyPath,
	const FString& ValueExportText, bool bFireChangeNotify)
{
	const auto Path = BridgePropertyOps::ParsePath(PropertyPath);
	for (UObject* Obj : BridgePropertyOps::ResolveCandidates(ObjectOrClassPath))
	{
		const auto Resolved = BridgePropertyOps::ResolvePath(Obj, Path);
		if (!Resolved.LeafProperty || !Resolved.LeafValuePtr) continue;

		FScopedTransaction Transaction(LOCTEXT("BridgeSetUProperty", "Bridge: Set UProperty"));
		Obj->Modify();

		const TCHAR* AfterParse = Resolved.LeafProperty->ImportText_Direct(
			*ValueExportText, Resolved.LeafValuePtr, Obj, PPF_None, GLog);
		if (!AfterParse)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("BridgeProperty: ImportText failed for '%s' = '%s'"),
				*PropertyPath, *ValueExportText);
			return false;
		}

		if (bFireChangeNotify)
		{
			BridgePropertyOps::FireChangeNotify(Obj, Resolved.PropertyChain);
		}
		return true;
	}
	return false;
}

bool UUnrealBridgePropertyLibrary::ArrayAppendUProperty(
	const FString& ObjectOrClassPath, const FString& PropertyPath,
	const FString& ElementExportText, bool bFireChangeNotify)
{
	const auto Path_ = BridgePropertyOps::ParsePath(PropertyPath);
	UObject* Obj = nullptr;
	BridgePropertyOps::FResolvedProp Resolved;
	for (UObject* C : BridgePropertyOps::ResolveCandidates(ObjectOrClassPath))
	{
		const auto R = BridgePropertyOps::ResolvePath(C, Path_);
		if (R.LeafProperty && R.LeafValuePtr) { Obj = C; Resolved = R; break; }
	}
	if (!Obj) return false;

	FScopedTransaction Transaction(LOCTEXT("BridgeArrayAppend", "Bridge: Array Append"));
	Obj->Modify();

	// Specialcase: FGameplayTagContainer — go through AddTag() to maintain ParentTags cache.
	FStructProperty* StructProp = CastField<FStructProperty>(Resolved.LeafProperty);
	if (StructProp && StructProp->Struct == FGameplayTagContainer::StaticStruct())
	{
		FGameplayTagContainer* Container = static_cast<FGameplayTagContainer*>(Resolved.LeafValuePtr);

		// Accept either a bare tag name "Combat.Hit" or struct-export "(TagName=\"Combat.Hit\")".
		FString TagName = ElementExportText.TrimStartAndEnd();
		const FString OpenWrap = TEXT("(TagName=\"");
		const FString CloseWrap = TEXT("\")");
		if (TagName.StartsWith(OpenWrap) && TagName.EndsWith(CloseWrap))
		{
			TagName = TagName.Mid(OpenWrap.Len(), TagName.Len() - OpenWrap.Len() - CloseWrap.Len());
		}

		const FGameplayTag NewTag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false);
		if (!NewTag.IsValid())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("BridgeProperty: ArrayAppend on FGameplayTagContainer — tag '%s' not registered"),
				*TagName);
			return false;
		}
		Container->AddTag(NewTag);

		if (bFireChangeNotify)
		{
			BridgePropertyOps::FireChangeNotify(Obj, Resolved.PropertyChain);
		}
		return true;
	}

	// Generic TArray
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Resolved.LeafProperty);
	if (!ArrayProp)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("BridgeProperty: ArrayAppend target '%s' is neither FArrayProperty nor FGameplayTagContainer"),
			*PropertyPath);
		return false;
	}

	FScriptArrayHelper Helper(ArrayProp, Resolved.LeafValuePtr);
	const int32 NewIdx = Helper.AddValue();
	void* NewElemPtr = Helper.GetRawPtr(NewIdx);

	const TCHAR* AfterParse = ArrayProp->Inner->ImportText_Direct(
		*ElementExportText, NewElemPtr, Obj, PPF_None, GLog);
	if (!AfterParse)
	{
		Helper.RemoveValues(NewIdx, 1);  // rollback
		UE_LOG(LogTemp, Warning,
			TEXT("BridgeProperty: ArrayAppend ImportText failed for '%s'"), *ElementExportText);
		return false;
	}

	if (bFireChangeNotify)
	{
		BridgePropertyOps::FireChangeNotify(Obj, Resolved.PropertyChain);
	}
	return true;
}

bool UUnrealBridgePropertyLibrary::ArrayRemoveUProperty(
	const FString& ObjectOrClassPath, const FString& PropertyPath,
	int32 Index, bool bFireChangeNotify)
{
	const auto Path_ = BridgePropertyOps::ParsePath(PropertyPath);
	UObject* Obj = nullptr;
	BridgePropertyOps::FResolvedProp Resolved;
	for (UObject* C : BridgePropertyOps::ResolveCandidates(ObjectOrClassPath))
	{
		const auto R = BridgePropertyOps::ResolvePath(C, Path_);
		if (R.LeafProperty && R.LeafValuePtr) { Obj = C; Resolved = R; break; }
	}
	if (!Obj) return false;

	FScopedTransaction Transaction(LOCTEXT("BridgeArrayRemove", "Bridge: Array Remove"));
	Obj->Modify();

	// FGameplayTagContainer specialcase — remove by index of GameplayTags array
	FStructProperty* StructProp = CastField<FStructProperty>(Resolved.LeafProperty);
	if (StructProp && StructProp->Struct == FGameplayTagContainer::StaticStruct())
	{
		FGameplayTagContainer* Container = static_cast<FGameplayTagContainer*>(Resolved.LeafValuePtr);
		TArray<FGameplayTag> Tags;
		Container->GetGameplayTagArray(Tags);
		const int32 RealIdx = Index < 0 ? Tags.Num() + Index : Index;
		if (RealIdx < 0 || RealIdx >= Tags.Num()) return false;
		Container->RemoveTag(Tags[RealIdx]);
		if (bFireChangeNotify) BridgePropertyOps::FireChangeNotify(Obj, Resolved.PropertyChain);
		return true;
	}

	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Resolved.LeafProperty);
	if (!ArrayProp) return false;

	FScriptArrayHelper Helper(ArrayProp, Resolved.LeafValuePtr);
	const int32 RealIdx = Index < 0 ? Helper.Num() + Index : Index;
	if (RealIdx < 0 || RealIdx >= Helper.Num()) return false;

	Helper.RemoveValues(RealIdx, 1);
	if (bFireChangeNotify) BridgePropertyOps::FireChangeNotify(Obj, Resolved.PropertyChain);
	return true;
}

bool UUnrealBridgePropertyLibrary::ArrayClearUProperty(
	const FString& ObjectOrClassPath, const FString& PropertyPath,
	bool bFireChangeNotify)
{
	const auto Path_ = BridgePropertyOps::ParsePath(PropertyPath);
	UObject* Obj = nullptr;
	BridgePropertyOps::FResolvedProp Resolved;
	for (UObject* C : BridgePropertyOps::ResolveCandidates(ObjectOrClassPath))
	{
		const auto R = BridgePropertyOps::ResolvePath(C, Path_);
		if (R.LeafProperty && R.LeafValuePtr) { Obj = C; Resolved = R; break; }
	}
	if (!Obj) return false;

	FScopedTransaction Transaction(LOCTEXT("BridgeArrayClear", "Bridge: Array Clear"));
	Obj->Modify();

	FStructProperty* StructProp = CastField<FStructProperty>(Resolved.LeafProperty);
	if (StructProp && StructProp->Struct == FGameplayTagContainer::StaticStruct())
	{
		FGameplayTagContainer* Container = static_cast<FGameplayTagContainer*>(Resolved.LeafValuePtr);
		Container->Reset();
		if (bFireChangeNotify) BridgePropertyOps::FireChangeNotify(Obj, Resolved.PropertyChain);
		return true;
	}

	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Resolved.LeafProperty);
	if (!ArrayProp) return false;
	FScriptArrayHelper Helper(ArrayProp, Resolved.LeafValuePtr);
	Helper.EmptyValues();
	if (bFireChangeNotify) BridgePropertyOps::FireChangeNotify(Obj, Resolved.PropertyChain);
	return true;
}

FString UUnrealBridgePropertyLibrary::GetAssetCDOPath(const FString& AssetPath)
{
	if (AssetPath.IsEmpty()) return FString();
	UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (!Loaded) return FString();
	if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
	{
		if (BP->GeneratedClass)
		{
			if (UObject* CDO = BP->GeneratedClass->GetDefaultObject())
			{
				return CDO->GetPathName();
			}
		}
	}
	if (UClass* Cls = Cast<UClass>(Loaded))
	{
		if (UObject* CDO = Cls->GetDefaultObject())
		{
			return CDO->GetPathName();
		}
	}
	// Already an instance — just echo back its path
	return Loaded->GetPathName();
}

#undef LOCTEXT_NAMESPACE
