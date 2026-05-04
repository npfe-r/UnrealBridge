#include "UnrealBridgeGameplayTagLibrary.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"

namespace BridgeGameplayTagOps
{
	/** EGameplayTagSourceType → short string. Centralised so both
	 *  GetTagSourceInfo and any future reverse lookup share names. */
	const TCHAR* SourceTypeToString(EGameplayTagSourceType Type)
	{
		switch (Type)
		{
			case EGameplayTagSourceType::Native:             return TEXT("Native");
			case EGameplayTagSourceType::DefaultTagList:     return TEXT("DefaultTagList");
			case EGameplayTagSourceType::TagList:            return TEXT("TagList");
			case EGameplayTagSourceType::RestrictedTagList:  return TEXT("RestrictedTagList");
			case EGameplayTagSourceType::DataTable:          return TEXT("DataTable");
			default:                                          return TEXT("Invalid");
		}
	}

	/** Resolve a TagSource record into a human-readable file location.
	 *  For ini-backed sources we pull the actual config file path; for
	 *  Native and DataTable we fall back to the source's FName. */
	FString ResolveSourceLocation(const FGameplayTagSource* Source)
	{
		if (!Source) return FString();

		switch (Source->SourceType)
		{
			case EGameplayTagSourceType::DefaultTagList:
			case EGameplayTagSourceType::TagList:
			case EGameplayTagSourceType::RestrictedTagList:
			{
				const FString ConfigPath = Source->GetConfigFileName();
				return ConfigPath.IsEmpty() ? Source->SourceName.ToString() : ConfigPath;
			}
			default:
				return Source->SourceName.ToString();
		}
	}
}

TArray<FString> UUnrealBridgeGameplayTagLibrary::FindAssetsReferencingTag(
	const FString& TagString, bool bIncludeChildren,
	const FString& PackagePathFilter, int32 MaxResults)
{
	TArray<FString> Result;
	if (TagString.IsEmpty()) return Result;

	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	const FGameplayTag RootTag = TagsMgr.RequestGameplayTag(FName(*TagString), /*ErrorIfNotFound=*/false);
	if (!RootTag.IsValid())
	{
		// Fall through and try the literal name anyway — useful for
		// finding stale references to a tag that has been deleted but
		// is still indexed in old assets.
		UE_LOG(LogTemp, Verbose,
			TEXT("FindAssetsReferencingTag: tag '%s' not registered; querying SearchableName by raw name only."),
			*TagString);
	}

	TArray<FName> TagsToQuery;
	TagsToQuery.Add(FName(*TagString));

	if (bIncludeChildren && RootTag.IsValid())
	{
		const FGameplayTagContainer ChildContainer = TagsMgr.RequestGameplayTagChildren(RootTag);
		for (const FGameplayTag& Child : ChildContainer)
		{
			TagsToQuery.AddUnique(Child.GetTagName());
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	UScriptStruct* TagStruct = FGameplayTag::StaticStruct();

	TSet<FString> Unique;
	for (const FName& TagName : TagsToQuery)
	{
		const FAssetIdentifier TagId(TagStruct, TagName);
		TArray<FAssetIdentifier> Refs;
		AssetRegistry.GetReferencers(TagId, Refs,
			UE::AssetRegistry::EDependencyCategory::SearchableName);

		for (const FAssetIdentifier& Ref : Refs)
		{
			if (Ref.PackageName.IsNone()) continue;
			const FString PackageName = Ref.PackageName.ToString();
			if (!PackagePathFilter.IsEmpty() && !PackageName.StartsWith(PackagePathFilter)) continue;
			Unique.Add(PackageName);
			if (MaxResults > 0 && Unique.Num() >= MaxResults) break;
		}
		if (MaxResults > 0 && Unique.Num() >= MaxResults) break;
	}

	Result = Unique.Array();
	Result.Sort();
	if (MaxResults > 0 && Result.Num() > MaxResults) Result.SetNum(MaxResults);
	return Result;
}

TArray<FString> UUnrealBridgeGameplayTagLibrary::ListAllRegisteredTags(
	const FString& FilterPrefix, int32 MaxResults)
{
	TArray<FString> Result;

	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	FGameplayTagContainer AllTags;
	TagsMgr.RequestAllGameplayTags(AllTags, /*OnlyIncludeDictionaryTags=*/false);

	for (const FGameplayTag& Tag : AllTags)
	{
		FString TagStr = Tag.ToString();
		if (!FilterPrefix.IsEmpty() && !TagStr.StartsWith(FilterPrefix)) continue;
		Result.Add(MoveTemp(TagStr));
		if (MaxResults > 0 && Result.Num() >= MaxResults) break;
	}

	Result.Sort();
	return Result;
}

FBridgeTagSourceInfo UUnrealBridgeGameplayTagLibrary::GetTagSourceInfo(const FString& TagString)
{
	FBridgeTagSourceInfo Info;
	Info.TagString = TagString;
	Info.SourceType = TEXT("NotFound");

	if (TagString.IsEmpty()) return Info;

	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	const TSharedPtr<FGameplayTagNode> Node = TagsMgr.FindTagNode(FName(*TagString));
	if (!Node.IsValid()) return Info;

	Info.bFound = true;
	Info.bIsRestricted = Node->IsRestrictedGameplayTag();

#if WITH_EDITORONLY_DATA
	Info.bIsExplicit = Node->IsExplicitTag();

	const TArray<FName>& SourceNames = Node->GetAllSourceNames();
	if (SourceNames.Num() == 0)
	{
		Info.SourceType = TEXT("Unknown");
		return Info;
	}

	// Primary = first source.
	const FName PrimarySourceName = SourceNames[0];
	if (const FGameplayTagSource* PrimarySource = TagsMgr.FindTagSource(PrimarySourceName))
	{
		Info.SourceType = BridgeGameplayTagOps::SourceTypeToString(PrimarySource->SourceType);
		Info.SourceLocation = BridgeGameplayTagOps::ResolveSourceLocation(PrimarySource);
	}
	else
	{
		Info.SourceType = TEXT("Unknown");
		Info.SourceLocation = PrimarySourceName.ToString();
	}

	// Additional sources (rare — when the same tag is registered from
	// multiple .ini / native locations).
	for (int32 i = 1; i < SourceNames.Num(); ++i)
	{
		Info.AdditionalSources.Add(SourceNames[i].ToString());
	}
#else
	// Non-editor build can't introspect source names. Should not happen
	// for this editor-only plugin, but guard for safety.
	Info.bIsExplicit = true;
	Info.SourceType = TEXT("Unknown");
#endif

	return Info;
}

// ── Tag source enumeration ──────────────────────────────────────

TArray<FBridgeTagSourceListing> UUnrealBridgeGameplayTagLibrary::ListTagSourceInis(
	const FString& FilterType)
{
	TArray<FBridgeTagSourceListing> Result;
	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();

	// FindTagSourcesWithType is the only public window onto the otherwise-private
	// TagSources map. Iterate every category and union the results.
	static const TArray<EGameplayTagSourceType> AllTypes = {
		EGameplayTagSourceType::Native,
		EGameplayTagSourceType::DefaultTagList,
		EGameplayTagSourceType::TagList,
		EGameplayTagSourceType::RestrictedTagList,
		EGameplayTagSourceType::DataTable,
	};

	const FName FilterFName = FilterType.IsEmpty() ? NAME_None : FName(*FilterType);

	for (EGameplayTagSourceType Type : AllTypes)
	{
		const FString TypeName = BridgeGameplayTagOps::SourceTypeToString(Type);
		if (!FilterFName.IsNone() && TypeName != FilterType) continue;

		TArray<const FGameplayTagSource*> Sources;
		TagsMgr.FindTagSourcesWithType(Type, Sources);

		for (const FGameplayTagSource* Source : Sources)
		{
			if (!Source) continue;
			FBridgeTagSourceListing Entry;
			Entry.SourceName = Source->SourceName.ToString();
			Entry.SourceType = TypeName;
			Entry.ConfigFilePath = BridgeGameplayTagOps::ResolveSourceLocation(Source);
			// Native sources require C++ edits — not writable from here.
			// DataTable would need DataTable-row mutation, also not yet wired.
			// Anything ini-backed (DefaultTagList / TagList / RestrictedTagList)
			// IS writable via IGameplayTagsEditorModule.
			switch (Type)
			{
				case EGameplayTagSourceType::DefaultTagList:
				case EGameplayTagSourceType::TagList:
				case EGameplayTagSourceType::RestrictedTagList:
					Entry.bIsWritable = true;
					break;
				default:
					Entry.bIsWritable = false;
					break;
			}
			Result.Add(MoveTemp(Entry));
		}
	}

	return Result;
}

// ── Mutations ───────────────────────────────────────────────────

bool UUnrealBridgeGameplayTagLibrary::AddGameplayTag(
	const FString& NewTag, const FString& SourceIni,
	const FString& Comment, bool bIsRestricted)
{
	if (NewTag.IsEmpty()) return false;
	if (!IGameplayTagsEditorModule::IsAvailable())
	{
		UE_LOG(LogTemp, Warning, TEXT("AddGameplayTag: GameplayTagsEditor module unavailable"));
		return false;
	}

	const FName SourceFName = SourceIni.IsEmpty() ? NAME_None : FName(*SourceIni);
	return IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(
		NewTag, Comment, SourceFName, bIsRestricted, /*bAllowNonRestrictedChildren=*/true);
}

bool UUnrealBridgeGameplayTagLibrary::RenameGameplayTag(
	const FString& OldTag, const FString& NewTag, bool bRenameChildren)
{
	// Case-sensitive: a case-only rename (e.g. "Foo.Bar" -> "foo.bar") is a legitimate
	// op that produces a redirect. FString::operator== is case-insensitive, so use Equals
	// with ESearchCase::CaseSensitive to let case-only renames through.
	if (OldTag.IsEmpty() || NewTag.IsEmpty() || OldTag.Equals(NewTag, ESearchCase::CaseSensitive)) return false;
	if (!IGameplayTagsEditorModule::IsAvailable())
	{
		UE_LOG(LogTemp, Warning, TEXT("RenameGameplayTag: GameplayTagsEditor module unavailable"));
		return false;
	}

	// UE's RenameTagInINI does NOT validate that OldTag exists — it cheerfully
	// writes a redirect for any string pair, leaving useless +GameplayTagRedirects
	// lines in the ini. Guard up-front: only rename real tags.
	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	if (!TagsMgr.FindTagNode(FName(*OldTag)).IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("RenameGameplayTag: '%s' not registered"), *OldTag);
		return false;
	}

	return IGameplayTagsEditorModule::Get().RenameTagInINI(OldTag, NewTag, bRenameChildren);
}

bool UUnrealBridgeGameplayTagLibrary::RemoveGameplayTag(const FString& TagString)
{
	if (TagString.IsEmpty()) return false;
	if (!IGameplayTagsEditorModule::IsAvailable())
	{
		UE_LOG(LogTemp, Warning, TEXT("RemoveGameplayTag: GameplayTagsEditor module unavailable"));
		return false;
	}

	UGameplayTagsManager& TagsMgr = UGameplayTagsManager::Get();
	const TSharedPtr<FGameplayTagNode> Node = TagsMgr.FindTagNode(FName(*TagString));
	if (!Node.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("RemoveGameplayTag: '%s' not in tag manager"), *TagString);
		return false;
	}

	return IGameplayTagsEditorModule::Get().DeleteTagFromINI(Node);
}
