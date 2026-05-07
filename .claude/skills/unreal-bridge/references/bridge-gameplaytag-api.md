# UnrealBridge GameplayTag Library API

`unreal.UnrealBridgeGameplayTagLibrary` — GameplayTag-specific helpers built on top of the AssetRegistry's SearchableName index and `UGameplayTagsManager`.

For generic SearchableName queries (PrimaryAssetId, GameplayCueTag, project-defined named-value structs), use the lower-level `UnrealBridgeAssetLibrary` functions instead — see `bridge-asset-api.md` "SearchableName Index". GameplayTag values flow through the same index; this library just adds tag-specific niceties:

- child-tag expansion (queries the tag tree via `UGameplayTagsManager`, then unions multi-tag results)
- access to *registered* tags including unused ones (SearchableName only sees referenced tags)
- source-of-definition lookup (which DataTable / .ini / native module declares a tag)

For runtime ASC tag queries (live actor tags, active GE tags), see `bridge-gameplayability-api.md`.

For tag hierarchy navigation (`find_child_tags` / `get_tag_parents`), also see `bridge-gameplayability-api.md` — those existed before this library.

---

## find_assets_referencing_tag(tag_string, include_children, package_path_filter, max_results) -> list[str]

Find every asset that references a GameplayTag — the programmatic equivalent of the editor's right-click → "Search for References" on a tag.

```python
gtl = unreal.UnrealBridgeGameplayTagLibrary

# Every asset referencing this exact tag
refs = gtl.find_assets_referencing_tag('Combat.Hit', False, '', 0)

# Include child tags too (Combat.Hit + Combat.Hit.Critical + Combat.Hit.Glance ...)
refs_recursive = gtl.find_assets_referencing_tag('Combat.Hit', True, '', 0)

# /Game-only, capped at 50
project_refs = gtl.find_assets_referencing_tag('Combat.Hit', True, '/Game', 50)
```

Parameters:

| name | meaning |
|---|---|
| `tag_string` | Full tag, e.g. `'Combat.Hit'`. |
| `include_children` | When `True`, walks the tag tree via `UGameplayTagsManager.RequestGameplayTagChildren` and unions results. When `False`, queries only the exact tag. |
| `package_path_filter` | Empty = no filter. Otherwise only return packages whose path starts with this — e.g. `'/Game'` to exclude engine + plugins. |
| `max_results` | `0` = unlimited. |

Returns sorted, de-duplicated package paths.

**Edge case**: when the tag has been deleted from the manager but stale assets still reference it, the query is best-effort — child expansion is skipped (the manager doesn't know the tree), but the literal-name query still finds the orphaned references.

---

## list_all_registered_tags(filter_prefix, max_results) -> list[str]

Every tag the `UGameplayTagsManager` knows about — including ones that have never been referenced by any asset (which the SearchableName index would miss). Backed by `RequestAllGameplayTags`.

```python
all_tags = gtl.list_all_registered_tags('', 0)
combat_branch = gtl.list_all_registered_tags('Combat.', 0)
```

**Difference from `list_searchable_name_values('GameplayTag', ...)`** (in `UnrealBridgeAssetLibrary`):

| function | source | what it returns |
|---|---|---|
| `list_all_registered_tags` | `UGameplayTagsManager` | all *registered* tags (config + native + DataTable + ini) |
| `list_searchable_name_values('GameplayTag', ...)` | AssetRegistry SearchableName | all tags *at least one asset has referenced* |

**Dead-tag detection**: set difference `registered − used` reveals tags that are defined but never referenced — candidates for cleanup. Combine the two queries client-side.

---

## get_tag_source_info(tag_string) -> BridgeTagSourceInfo

Where is this tag defined? Use this when planning to rename / delete a tag — answers "which .ini or DataTable do I edit?".

```python
info = gtl.get_tag_source_info('Foley.Event.Jump')
print(info.source_type)      # "DefaultTagList"
print(info.source_location)  # "C:/.../Config/DefaultGameplayTags.ini"
print(info.is_explicit)      # True
print(info.found)            # True
```

`BridgeTagSourceInfo` fields:

| field | type | meaning |
|---|---|---|
| `tag_string` | str | The tag the lookup was performed for (echoes input). |
| `found` | bool | False when the tag isn't in the manager (typo, plugin not loaded, deleted tag). All other fields are unreliable when False. |
| `source_type` | str | `"Native"`, `"DefaultTagList"`, `"TagList"`, `"RestrictedTagList"`, `"DataTable"`, `"NotFound"`, `"Unknown"`. |
| `source_location` | str | For ini sources: the actual config file path. For Native / DataTable: the source FName (usually module / asset name). |
| `is_explicit` | bool | True if the tag was explicitly added; False if it only exists as an implicit parent of a more specific child tag. |
| `is_restricted` | bool | True if this is a restricted gameplay tag. |
| `additional_sources` | list[str] | Rare: when the same tag is registered from multiple places (e.g. native + ini), the secondary source FNames are listed here. The primary one is in `source_type` / `source_location`. |

Source type semantics:

| source_type | meaning |
|---|---|
| `Native` | Defined via `UE_DEFINE_GAMEPLAY_TAG` / `FNativeGameplayTag` C++ macros |
| `DefaultTagList` | `Config/DefaultGameplayTags.ini` (Project Settings → GameplayTags) |
| `TagList` | Other `.ini` file under `Config/Tags/` |
| `RestrictedTagList` | Restricted-tag `.ini` |
| `DataTable` | Declared in a UDataTable asset |
| `NotFound` | Tag isn't in the manager at all |
| `Unknown` | Found in manager but couldn't resolve source (rare) |

---

---

## Tag source enumeration

### list_tag_source_inis(filter_type) -> list[BridgeTagSourceListing]

Every place this project's tags can come from — ini files, native code modules, DataTables. Pass the returned `source_name` as the `source_ini` argument to `add_gameplay_tag(...)` to control where a new tag lands.

```python
sources = gtl.list_tag_source_inis('')           # all sources
inis    = gtl.list_tag_source_inis('DefaultTagList')  # just the project's primary ini
for s in sources:
    print(s.source_name, s.source_type, 'writable' if s.is_writable else '(read-only)')
```

`filter_type` accepts: `''`, `'Native'`, `'DefaultTagList'`, `'TagList'`, `'RestrictedTagList'`, `'DataTable'`.

`BridgeTagSourceListing` fields:

| field | meaning |
|---|---|
| `source_name` | FName-as-string. Pass to `add_gameplay_tag(source_ini=...)`. |
| `source_type` | One of the EGameplayTagSourceType strings above. |
| `config_file_path` | Resolved on-disk path for ini-backed sources; empty for Native/DataTable. |
| `is_writable` | True when the bridge's mutation APIs can target this source (DefaultTagList / TagList / RestrictedTagList only). Native = C++ source edit required; DataTable = use the DataTable library. |

---

## Mutations

These three write to disk via `IGameplayTagsEditorModule`. Native-source tags can't be written from Python — they live in C++ source. Restricted tags use a separate flow not yet exposed.

### add_gameplay_tag(new_tag, source_ini='', comment='', is_restricted=False) -> bool

Add a new GameplayTag. `source_ini=''` writes to the project's default (`Config/DefaultGameplayTags.ini`); pass a `source_name` from `list_tag_source_inis(...)` to target a specific ini.

```python
ok = gtl.add_gameplay_tag('Combat.Stun', '', 'Stuns the target for N seconds')
# Or target a specific source:
ok = gtl.add_gameplay_tag('UI.Modal.Inventory', 'MyMod.ini', '')
```

Returns False if the tag already exists, the source isn't found, or the editor module rejected the write.

### rename_gameplay_tag(old_tag, new_tag, rename_children=True) -> bool

Rename a tag in-place. UE auto-inserts a `+GameplayTagRedirects=(OldTagName=...,NewTagName=...)` line so existing asset references continue to resolve to the new name — **don't** also write the redirect manually.

```python
ok = gtl.rename_gameplay_tag('Combat.Hit', 'Combat.HitImpact', rename_children=True)
# rename_children=True also renames Combat.Hit.Critical → Combat.HitImpact.Critical
```

This wrapper validates `old_tag` is registered before delegating to UE — without that guard, `IGameplayTagsEditorModule::RenameTagInINI` would happily write a redirect for a non-existent tag, leaving useless `+GameplayTagRedirects=` litter in the ini. Returns False if `old_tag` doesn't exist.

### list_gameplay_tag_redirects(source_ini_filter='', old_tag_prefix_filter='') -> list[BridgeTagRedirectEntry]

Enumerate every `+GameplayTagRedirects=` entry in the project. Use to drive enumerate-then-sweep workflows without having to remember (old, new) pairs.

```python
# Every redirect across all writable sources
all_redirects = gtl.list_gameplay_tag_redirects('', '')
for r in all_redirects:
    print(f'{r.source_name}: {r.old_tag} -> {r.new_tag}')

# All Combat.* redirects in DefaultGameplayTags.ini
combat = gtl.list_gameplay_tag_redirects('DefaultGameplayTags.ini', 'Combat.')

# Sweep every orphan under Foley.* in one pass
for o in gtl.list_gameplay_tag_redirects('', 'Foley.'):
    gtl.remove_gameplay_tag_redirect(o.old_tag, o.new_tag)
```

`source_ini_filter` defaults to all writable sources; `old_tag_prefix_filter` defaults to no filter (and is **case-sensitive** to match GameplayTag semantics — `Statetree.` does NOT match `StateTree.`). Restricted-tag sources don't carry redirects in this UE version, so they're never enumerated.

`BridgeTagRedirectEntry` fields: `old_tag`, `new_tag`, `source_name` (the FName the manager indexes the source by — pass to `remove_gameplay_tag_redirect` paired with old/new).

### remove_gameplay_tag_redirect(old_tag, new_tag) -> bool

Drop a `+GameplayTagRedirects=(OldTagName="X",NewTagName="Y")` entry from the project's tag config. Use when undoing a mistaken rename or sweeping an orphan redirect.

```python
# Undo a mistaken rename:
gtl.rename_gameplay_tag('Combat.Hit', 'Combat.HitTypo', True)  # oops
gtl.rename_gameplay_tag('Combat.HitTypo', 'Combat.Hit', True)  # restore the right name
gtl.remove_gameplay_tag_redirect('Combat.HitTypo', 'Combat.Hit')  # clean the wrong redirect

# Sweep an orphan left over after a manual delete:
gtl.remove_gameplay_tag_redirect('Combat.OldUnused', 'Combat.New')
```

Walks every writable source ini for a redirect matching **both** `old_tag` and `new_tag` (the pair must match exactly — typo guard preventing accidental deletes). Removes from `UGameplayTagsList::GameplayTagRedirects` (in-memory) **and** strips the line from disk, then calls `EditorRefreshGameplayTagTree()` so subsequent lookups for `old_tag` correctly fail in the same session.

Returns `False` when no redirect with that exact (old, new) pair exists. Idempotent — calling twice on the same redirect just returns `False` the second time.

### remove_gameplay_tag(tag) -> bool

Delete a tag from its source ini.

```python
# ALWAYS check references first — UE doesn't insert a redirect on delete,
# so dangling references will surface as warnings on next load.
refs = gtl.find_assets_referencing_tag('Combat.OldTag', True, '/Game', 0)
if not refs:
    gtl.remove_gameplay_tag('Combat.OldTag')
```

Returns False if the tag doesn't exist or is native-defined.

**Caveats common to all three mutations:**
- Run from the editor (not cooked builds — these APIs need `IGameplayTagsEditorModule`).
- Changes write to disk **immediately** but the manager's in-memory tree refresh is what makes subsequent calls see the new state — which happens automatically.
- The editor periodically saves its in-memory tag state back to source inis. Direct manual edits to a tag ini while the editor is running risk being overwritten on shutdown — prefer these APIs over hand-editing the ini in a running session.
- After `rename_*` followed by `remove_*` of the new name, the redirect line for `old → new` is also cleaned up by UE. After a manual delete of the source-ini line in a stopped editor, dangling redirects can accumulate — clean them by hand.

---

## Known gotchas

Things that bit us during development — saved here so they don't bite you again.

### Index / lookup

- **SearchableName only sees on-disk asset references.** Tags added at runtime (PIE, transient editor state, C++ that calls `RequestGameplayTag` without storing it on a saved asset) don't appear in `find_assets_referencing_tag` results. Cross-check with a `grep` of `Source/` if you suspect C++ usage.
- **A reference inside a level actor instance surfaces as the level package**, not the actor. Loading the level and inspecting actors is the only way to localise further.
- **Cold-start race.** `find_assets_referencing_tag` returns `[]` if the AssetRegistry hasn't finished its initial scan. Allow a few seconds after editor launch (`bridge.py exec "1+1"` succeeds → registry is usually ready).
- **`is_explicit=False` tags are implicit parents.** `Combat` exists in the tree because `Combat.Hit` was registered, but `Combat` itself has no `+GameplayTagList=` line. Don't try to remove it directly — delete its last explicit child and the parent vanishes automatically.

### Mutations

- **Native tags are immutable from Python.** Tags declared via `UE_DEFINE_GAMEPLAY_TAG` / `FNativeGameplayTag` live in C++ source; `add` / `rename` / `remove` will fail. `list_tag_source_inis(...)` exposes `is_writable=False` for these — check before attempting.
- **DataTable-source tags also can't be mutated here.** Edit the DataTable asset directly via `UnrealBridgeDataTableLibrary` (planned). `is_writable=False` for these too.
- **Don't hand-edit a source ini while the editor is running.** UE periodically serialises its in-memory tag state back to the ini and will overwrite your manual edits on shutdown. Use the bridge mutation APIs (which go through the editor module and round-trip cleanly) or close the editor first.
- **`rename_gameplay_tag` validates `OldTag` exists; the raw `IGameplayTagsEditorModule::RenameTagInINI` does not.** Raw UE will cheerfully write `+GameplayTagRedirects=` lines for non-existent tags, leaving litter. Always go through the bridge wrapper, not raw `unreal` calls.
- **`remove_gameplay_tag` doesn't insert a redirect.** Use `find_assets_referencing_tag(tag, include_children=True)` first to confirm zero references, otherwise dangling references will surface as warnings on next load.
- **Redirect persistence is auto-hardened by the bridge.** UE 5.7 has a quirk where some `Add` / `Rename` / `Remove` mutations re-serialise `UGameplayTagsSettings` and silently drop on-disk `+GameplayTagRedirects=` lines whose `OldTagName` has no live in-memory tag node — exactly the shape every just-renamed redirect has. The bridge wrappers compensate by reading the source ini after every mutation and re-appending any redirect that's in the in-memory `UGameplayTagsList::GameplayTagRedirects` array but missing from disk. So in normal use you don't have to think about this — but if you bypass the wrapper and call `IGameplayTagsEditorModule::*` directly, the redirect can vanish silently and surface only on the next editor restart.
- **`remove_gameplay_tag` does NOT delete its source's redirects.** I previously documented "rename + remove auto-cleans the redirect" — that was inferred from a single observation and **doesn't hold in general**. Orphan redirects (where the OldTag is no longer registered) only get cleaned up on editor restart serialization, and even then it's not guaranteed. Sweep them by hand if your project's tag config matters.
- **Empty `source_ini=''` writes to `Config/DefaultGameplayTags.ini`.** This is the project's default and the only writable source on most projects. To use a different ini, first call `list_tag_source_inis('TagList')` (or `'RestrictedTagList'`) to see the available targets.

### Bridge USTRUCT field names (Python side)

- **The `b` prefix on bool fields is stripped by UE Python.** `bIsExplicit` / `bIsRestricted` / `bFound` / `bIsWritable` in C++ become `.is_explicit` / `.is_restricted` / `.found` / `.is_writable` in Python. The preflight will warn and suggest the right name; trust it. (See `feedback_ue_python_bool_prefix.md`.)

---

## Common workflows

**"What references this tag I'm about to rename?"**

```python
refs = gtl.find_assets_referencing_tag('Old.Tag.Name', True, '/Game', 0)
for r in refs: print(r)
src = gtl.get_tag_source_info('Old.Tag.Name')
print(f'Defined in: {src.source_location}')
```

**"Which tags are defined but never used?" (dead tags)**

```python
asl = unreal.UnrealBridgeAssetLibrary
registered = set(gtl.list_all_registered_tags('', 0))
used       = set(asl.list_searchable_name_values('GameplayTag', '', 0))
dead = sorted(registered - used)
print(f'{len(dead)} dead tags:'); [print(f'  {t}') for t in dead]
```

**"Which assets in /Game reference any tag under Combat.*?"**

```python
refs = gtl.find_assets_referencing_tag('Combat', True, '/Game', 0)
```

**"What tags does this Blueprint actually use?"**

```python
asl = unreal.UnrealBridgeAssetLibrary
tags = asl.get_searchable_names_used_by_asset('/Game/BP/MyAbility', 'GameplayTag', 0)
for t in tags: print(t.value_name)
```
