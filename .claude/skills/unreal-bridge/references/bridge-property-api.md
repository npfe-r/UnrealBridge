# UnrealBridge Property Library API

`unreal.UnrealBridgePropertyLibrary` — generic UPROPERTY read / write surface that operates one level **below** UE Python's `get_editor_property` / `set_editor_property` (directly on `FProperty` pointers in C++). Use it when:

- The field UE Python rejects with `"is protected and cannot be read/set"` (UE Python's check is misleading — see "What protected actually means" below)
- You need to write an `EditDefaultsOnly` sub-field of a struct value (UE Python's struct-copy path loses CDO context and rejects the write — the magnitude case)
- You want the **full reflection picture** including `private:` fields, bare `UPROPERTY()` with no `Edit*`/`Visible*`/`Blueprint*`, raw EPropertyFlags decoded, full metadata map

> ⚠️ **Privileged.** These APIs intentionally bypass UE editor's safety checks. The C++ access modifiers and `EditDefaultsOnly` flags exist for invariant protection — abusing `set_uproperty` on a `private:` field can corrupt class state. Read what you're touching first.

---

## list_uproperties(object_or_class_path, include_inherited) -> list[BridgeUPropertyInfo]

Full reflection dump. Includes `private` / `protected` / bare `UPROPERTY()` fields that `dir()` and `get_editor_property` don't expose to Python.

```python
P = unreal.UnrealBridgePropertyLibrary

# Asset CDO — most common
fields = P.list_u_properties("/Game/.../GE_BridgeTest", True)

# Class CDO
fields = P.list_u_properties("/Script/Engine.Actor", True)
```

Each `FBridgeUPropertyInfo`:

| field | meaning |
|---|---|
| `name` | PascalCase C++ name, e.g. `"Modifiers"` |
| `type_name` | UE C++ type, e.g. `"FString"` / `"TArray"` / `"FGameplayTagContainer"` |
| `declaring_class_path` | Class that DECLARED this property (parent class for inherited) |
| `cpp_access` | `"Public"` / `"Protected"` / `"Private"` / `"Unknown"` from `EPropertyFlags::CPF_NativeAccessSpecifier*` |
| `flags` | Decoded list, e.g. `["EditDefaultsOnly", "BlueprintReadOnly"]` |
| `metadata` | Raw `{Category, ToolTip, EditCondition, ...}` map |
| `is_container` | True for `TArray` / `TMap` / `TSet` |
| `is_struct_value` | True for nested `USTRUCT` (recurse via dotted path) |

---

## get_uproperty_as_export_text(object_or_class_path, property_path) -> (text, success)

Read any UPROPERTY by dotted path. Returns export-text (UE-native serialization, round-trips for any type).

```python
v, ok = P.get_u_property_as_export_text(
    "/Game/.../GE_BridgeTest",
    "Modifiers[0].ModifierMagnitude.ScalableFloatMagnitude.Value")
# v == "2.163000"  ok == True

v, ok = P.get_u_property_as_export_text("/Game/MyAsset", "Tags")
# v == "(Tag1,Tag2,Tag3)"
```

**Path grammar**: dotted nesting + `[N]` array indexing. Negative indices count from end (`[-1]` = last).

---

## set_uproperty_from_export_text(object_or_class_path, property_path, value, fire_change_notify=True) -> bool

Write any UPROPERTY from export-text. Bypasses UE Python's "cannot be edited on instances" gate that blocks `EditDefaultsOnly` sub-fields of struct copies.

```python
# The motivating case: edit GE modifier magnitude
P.set_u_property_from_export_text(
    "/Game/.../GE_BridgeTest",
    "Modifiers[0].ModifierMagnitude.ScalableFloatMagnitude.Value",
    "50.0", fire_change_notify=True)
```

Behavior:
- Wraps in `FScopedTransaction` (Ctrl+Z works)
- Calls `Object->Modify()` to mark dirty
- When `fire_change_notify=True`: calls `PostEditChangeChainProperty` with the full path chain → open editor windows refresh in real time

**`fire_change_notify=False` is for batch edits** — call several writes with `False`, then a final write or no-op write with `True` so editor sees a single coherent update instead of intermediate states. Useful for tagged-union writes:

```python
# Switch ModifierMagnitude variant from ScalableFloat to AttributeBased
P.set_u_property_from_export_text(GE, "Modifiers[0].ModifierMagnitude.MagnitudeCalculationType",
                                   "AttributeBased", fire_change_notify=False)
P.set_u_property_from_export_text(GE, "Modifiers[0].ModifierMagnitude.AttributeBasedMagnitude.Coefficient.Value",
                                   "1.5", fire_change_notify=True)  # last call notifies
```

---

## array_append / array_remove / array_clear

Container-mutation primitives for `TArray<T>` and `FGameplayTagContainer`.

```python
# Append element (whole struct via export-text)
P.array_append_u_property(GE, "Modifiers", existing_modifier_text, True)

# Append a tag — auto-detected FGameplayTagContainer routes through
# Container.AddTag() so the internal ParentTags cache stays consistent.
P.array_append_u_property("/Game/MyEffect", "GrantedTags", "Combat.Hit", True)

# Remove by index (negative counts from end)
P.array_remove_u_property(GE, "Modifiers", -1, True)

# Clear all elements
P.array_clear_u_property(GE, "Modifiers", True)
```

Element export-text formats:
- `TArray<float>`: `"1.5"`
- `TArray<FVector>`: `"(X=1,Y=2,Z=3)"`
- `TArray<FGameplayModifierInfo>`: full struct text `"(Attribute=...,...)"` — easiest is to read another element first via `get_uproperty_as_export_text` and copy
- `FGameplayTagContainer`: bare tag name `"Combat.Hit"` OR `"(TagName=\"Combat.Hit\")"`

`TMap` and `TSet` are not yet supported by these container ops; use `set_uproperty_from_export_text` to overwrite the whole map/set if needed.

---

## get_asset_cdo_path(asset_path) -> str

Resolve asset path to its real CDO object path. Solves the trap where `bp.generated_class().get_default_object()` in UE Python sometimes returns the BPGC meta-instance instead of the `Default__Foo_C` CDO.

```python
cdo = P.get_asset_cdo_path("/Game/.../GE_BridgeTest")
# cdo == "/Game/.../GE_BridgeTest.Default__GE_BridgeTest_C"
```

The other functions accept either form — pass either the asset path OR the explicit CDO path; they auto-resolve. This helper is for cases where you need to hand the CDO path to other UE Python APIs.

---

## Path resolution semantics

Functions accept three path formats:

| Format | Example | Resolves to |
|---|---|---|
| Asset content path | `/Game/Foo/Bar` | Blueprint asset → its CDO (primary), with the UBlueprint asset as fallback for asset-level fields like `ParentClass` |
| Explicit object path | `/Game/Foo/Bar.Default__Bar_C` | That exact UObject |
| UClass path | `/Script/Engine.Actor` | The class's CDO |

The **fallback** matters: when reading `ParentClass` on a Blueprint asset path, the read first tries the CDO (where `ParentClass` doesn't exist), then automatically retries on the UBlueprint asset (where `ParentClass` lives). This means asset-level fields and runtime-class fields are both reachable from the same path string.

---

## Known gotchas

### What "protected" actually means in UE Python's error

When UE Python's `get_editor_property` says `"is protected and cannot be read"`, the word **"protected" doesn't mean C++ `protected:`** — it means "this property isn't `BlueprintVisible` so the Python binding refuses". Many fields are C++ `protected:` but show `cpp_access="Public"` in `list_uproperties` because UE's reflection layer doesn't track the raw C++ access modifier unless explicitly tagged. Check `flags` for the absence of `BlueprintReadWrite`/`BlueprintReadOnly` to predict whether UE Python will accept access.

### Tagged-union structs need lockstep writes

`FGameplayEffectModifierMagnitude` has 4 variant fields (`ScalableFloatMagnitude` / `AttributeBasedMagnitude` / `CustomMagnitude` / `SetByCallerMagnitude`) with one discriminator (`MagnitudeCalculationType`). The bridge does NOT enforce that the discriminator matches the active variant — caller's responsibility to set both in lockstep (see `fire_change_notify=False` example above for the recommended pattern).

### PostEditChangeProperty has side effects

`fire_change_notify=True` runs the property's PostEdit callback, which may rebuild data structures, refresh editor windows, mark dependent dirty, etc. For most edits this is desired (editor stays in sync). For high-frequency batch writes consider `False` for intermediate writes.

### CDO writes need `save_asset` to persist

Writes hit the in-memory CDO + transaction stack, but the `.uasset` file isn't touched until you call `unreal.EditorAssetLibrary.save_asset(asset_path)`. Closing the editor without saving discards the changes. (We hit this during smoke testing — the GE's modifier disappeared across an editor restart because it was added in-editor but never saved to disk.)

### TMap / TSet write isn't in v1

`TArray<T>` and `FGameplayTagContainer` are first-class. For `TMap` / `TSet`, write the whole container as export-text via `set_uproperty_from_export_text`.

### Don't bypass for safety-critical fields

`private:` / `protected:` exist for invariant protection. Writing to them via this API works but can corrupt class state. Prefer the dedicated bridge APIs (`add_gameplay_tag`, `add_ge_modifier_scalable`, etc.) when they exist — they handle the side effects correctly.
