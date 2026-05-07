# Enhanced Input + InputAxis 拓展路线图

> 范围：把 bridge 在 EnhancedInput（IA / IMC / Trigger / Modifier / 蓝图节点 / Pawn 装配）
> 与遗留 InputAxis & ActionMappings 这两块从"能跑能查"推进到"能从零搭、能调、能验证"。
>
> 写于 2026-05-07。已交付参考 `feat(input): Enhanced Input authoring`（79fb462）+
> `feat(perf)` 系列后的代码状态。

---

## 1. 现状（已交付，作为基线）

`UnrealBridgeGameplayLibrary` 已有：

- **资产枚举**：`ListInputActions(filter, max)` / `ListInputMappingContexts(filter, max)`
- **IMC 映射读写**：`GetInputMappingContextMappings(imc)` →
  `[{action_path, action_name, key_name, trigger_classes[], modifier_classes[]}]`；
  `AddIAMappingToIMC(imc, ia, key)` / `RemoveIAMappingFromIMC(imc, ia, key)`
  （都包 `FScopedTransaction`，但 **trigger / modifier 实例是空的**）
- **IA 元数据读**：`GetInputActionValueType(ia)` → `Boolean/Axis1D/Axis2D/Axis3D`；
  `GetInputActionTriggers(ia, ...)` → 类路径 + 关键参数
- **运行时注入**：`InjectEnhancedInputAxis(ia, vec)` 一帧；
  `SetStickyInput / ClearStickyInput`（多帧持续）；
  `TriggerInputAction(ia, hold_seconds)`（自动按 trigger 类挑 pulse vs. timed hold）
- **MappingContext 栈控制**：`AddMappingContext(imc, priority)` /
  `RemoveMappingContext(imc)` / `IsMappingContextActive(imc)`
- **响应式订阅**：`UnrealBridgeReactiveLibrary.RegisterRuntimeInputAction(...)`
  via `BridgeReactiveAdapter_InputAction` —— Python 端 callback 收 IA 触发

`UnrealBridgeBlueprintLibrary` 已有但与 Input 弱相关：

- `AddNodeByClassName(class_path, x, y)` —— 通用 K2Node 构造，但**对
  `K2Node_EnhancedInputAction` / `K2Node_InputAction` 不够**：
  这两类节点必须在 `AllocateDefaultPins` 之前先把 `InputAction` (UObject*)
  或 `InputActionName` (FName) UPROPERTY 写好，否则 Triggered/ActionValue
  这些 pin 根本不存在，节点是个裸壳，连不了线。
- `AddEventNode(parent, event_name)` —— 只走 UFUNCTION 派生事件
  （ReceiveBeginPlay 这种），EnhancedInput 事件不走这条路径。

---

## 2. 缺口分类

### A. 资产作图（IA / IMC 创建 + 实例属性）

| # | 项目 | 工程量 | 频次 | 说明 |
|---|---|---|---|---|
| A1 | **CreateInputAction(path, value_type, description)** | 小 | 高 | `UAssetToolsImpl::CreateAsset` + `UInputAction::ValueType = ...`。是从零搭输入栈的入口。 |
| A2 | **CreateInputMappingContext(path, description)** | 小 | 高 | 同上，`UInputMappingContext` factory。 |
| A3 | **SetInputActionProperty(ia, key, value)** | 小 | 中 | 写 `bConsumeInput` / `bTriggerWhenPaused` / `bReserveAllMappings`（5.7+） / `Description`。`SetEditorProperty` 包 `PostEditChangeProperty`。 |
| A4 | **AddTriggerToIA(ia, trigger_class, params_json)** | 中 | 高 | 把 `UInputTriggerHold` / `Pulse` / `Tap` / `Pressed` / `Released` / `Chord` / `ChordAction` 实例追加到 `IA->Triggers[]`，按 JSON 写 `HoldTimeThreshold` 等参数。**79fb462 commit 显式 out-of-scope #2 的 IA 侧。** |
| A5 | **RemoveTriggerFromIA(ia, index_or_class)** | 小 | 中 | 与 A4 对称，按下标或类名删；空参 = 清空。 |
| A6 | **AddModifierToIA(ia, modifier_class, params_json)** | 中 | 中-高 | `UInputModifierNegate` / `Scalar` / `DeadZone` / `Smooth` / `SwizzleAxis` / `ToWorldSpace`，写 `IA->Modifiers[]`。 |
| A7 | **AddTriggerToIMCMapping(imc, ia, key, trigger_class, params)** | 中 | 高 | **per-mapping** trigger（覆盖 IA 默认），`FEnhancedActionKeyMapping` 数组里的元素属性。**79fb462 out-of-scope #2 的 IMC 侧。** |
| A8 | **AddModifierToIMCMapping(imc, ia, key, modifier_class, params)** | 中 | 高 | 同上，per-mapping modifier。 |
| A9 | **SetIMCMappingPlayerMappableSettings(imc, ia, key, name, category)** | 小-中 | 中 | 5.3+ `PlayerMappableKeySettings`，玩家重绑定 UI 的源数据。无 bridge 封装时 raw 写很容易漏 `PostEditChangeProperty`。 |
| A10 | **DuplicateIMC(src, dst)** + **DuplicateIA(src, dst)** | 小 | 中 | 模板化常用，配 A1/A2。 |

### B. 蓝图图层节点（事件节点 + 取值节点）

| # | 项目 | 工程量 | 频次 | 说明 |
|---|---|---|---|---|
| B1 | **AddEnhancedInputActionEventNode(bp, graph, ia_path, x, y)** | 小-中 | **极高** | `NewObject<UK2Node_EnhancedInputAction>` → `Node->InputAction = LoadObject<UInputAction>(ia_path)` → `Graph->AddNode` → `AllocateDefaultPins` → `ReconstructNode`。返回 NodeGuid + 完整 pin 列表（5 个 exec + ActionValue/ElapsedSeconds/TriggeredSeconds），让后续 `connect_pins` 能用。**这是用户反馈最直接对应的项**。 |
| B2 | **AddInputActionEventNode(bp, graph, action_name, x, y)** | 小 | 中 | 旧 `K2Node_InputAction`，FName 绑定，5 个 exec pin (Pressed/Released)。给老项目用。 |
| B3 | **AddInputAxisEventNode(bp, graph, axis_name, x, y)** | 小 | 中 | 旧 `K2Node_InputAxisEvent` —— 单 float 输出 pin "Axis Value"。 |
| B4 | **AddInputKeyEventNode(bp, graph, key, x, y)** | 小 | 中 | `K2Node_InputKey`，绑特定 `FKey`（"SpaceBar"），不通过 IA。Pawn BP 里"按 K 触发调试"这种快路径常用。 |
| B5 | **AddInputAxisKeyEventNode(bp, graph, key, x, y)** | 小 | 低 | `K2Node_InputAxisKeyEvent`，给鼠标轴 / 摇杆轴单独连。 |
| B6 | **AddGetInputActionValueNode(bp, graph, x, y, swizzle?)** | 小 | 高 | `K2Node_GetInputActionValue` / 或纯 `BreakInputActionValue` —— 把 `FInputActionValue` 拆出 bool / Axis1D / 2D / 3D。EventNode 输出的 ActionValue 几乎一定要走这一步。 |
| B7 | **AddBindActionCallNode(bp, graph, ia_path, trigger_event, target_function, x, y)** | 中 | 高 | C++ Pawn 等价物的蓝图版：在 BP 里调 `EnhancedInputComponent::BindAction(IA, ETriggerEvent, target, FunctionName)`。等同于 `add_function_call_node` 但帮你填好 IA 和枚举 pin 默认值，免去手动调 K2 schema。 |

> **共同实现要点**：所有 B 系列节点必须在 `AllocateDefaultPins` 前把"绑定字段"
> （`InputAction` / `InputActionName` / `InputAxisName` / `InputKey`）填好，否则 pin 集合不对。
> 全部要 `MarkBlueprintAsStructurallyModified` 触发后续 compile。

### C. Pawn 装配 / Bind 路径

| # | 项目 | 工程量 | 频次 | 说明 |
|---|---|---|---|---|
| C1 | **FindBindActionCallSites(ia_path?)** | 小-中 | 高 | 跨 BP 扫所有 `K2Node_CallFunction` 调 `UEnhancedInputComponent::BindAction`，返回 `[(bp, graph, node_guid, ia_path, trigger_event, target_func)]`。**79fb462 out-of-scope #3。** |
| C2 | **FindEnhancedInputEventNodes(ia_path?)** | 小 | 高 | 同上但扫 `K2Node_EnhancedInputAction`，定位"哪些 BP 直接监听了这个 IA"。 |
| C3 | **FindAddMappingContextCallSites(imc_path?)** | 小 | 中 | 扫 `EnhancedInputLocalPlayerSubsystem::AddMappingContext` 的 BP 调用点 —— 哪个 PlayerController/Pawn 在哪个事件里 push 了 IMC，常见 bug 来源。 |
| C4 | **ScaffoldEnhancedInputPawn(bp_path, imc_path, ia_action_map)** | 中-大 | 高 | 一键脚手架：在指定 Pawn BP 里 (1) 重写 `PossessedBy` / `BeginPlay` 事件 (2) Cast Controller → PlayerController → 取 LocalPlayer Subsystem → AddMappingContext (3) 重写 `SetupPlayerInputComponent`(EnhancedInputComponent) 并按 `ia_action_map = {ia_path: (trigger_event, function_name)}` 批量生成 BindAction 调用。把"agent 写一个能玩的 Pawn"从 30+ 步骤压缩成一行。 |
| C5 | **ConvertLegacyInputBindingToEnhanced(bp)** | 大 | 低 | 旧 `K2Node_InputAction`/`InputAxisEvent` → IA + IMC + EnhancedInputAction event 节点的图迁移。难，但承接 5.0 之前的项目。优先级低于其他。 |

### D. 查询 / 用法发现 / 校验

| # | 项目 | 工程量 | 频次 | 说明 |
|---|---|---|---|---|
| D1 | **FindIAReferences(ia_path)** | 小 | 高 | 一站式：返回 `{imcs:[...], event_nodes:[...], bind_actions:[...], reactive_handlers:[...]}`。删 IA 前必跑，避免留下空引用。 |
| D2 | **FindIMCReferences(imc_path)** | 小 | 中 | AddMappingContext 调用点 + `DefaultPawnInputMappingContext` 这种引用字段。 |
| D3 | **ValidateInputBindings(scope)** | 中 | 中 | 检测：(a) IA 资产被 IMC 引用但已被删；(b) IMC 里 mapping 指向已删的 IA；(c) Pawn BP 的 BindAction 节点 IA 引用为空；(d) `K2Node_EnhancedInputAction` 节点 InputAction 字段为 null（典型 bridge 误用症状）。返回结构化报告。 |
| D4 | **DetectKeyConflicts(imcs[])** | 中 | 中 | 给一组 IMC（可加 priority），找同时激活时同一个 key 触发多个 IA 的冲突；或同 IMC 内一个 key 绑了 N 个 IA。 |
| D5 | **DetectTriggerConflicts(ia)** | 小 | 中 | 同一 IA 上 Hold + Tap + Pressed 互斥语义提示，或多 Chord trigger 链不闭合。规则比较简单可在 Python 侧做，但需要 D5 的结构化数据来源。 |

### E. 运行时调试 / PIE 追踪

| # | 项目 | 工程量 | 频次 | 说明 |
|---|---|---|---|---|
| E1 | **GetActiveMappingContextStack()** | 小 | 高 | PIE 时返回当前 LocalPlayer Subsystem 上的 IMC 栈：`[(imc_path, priority, registered_player_input)]`。配 `IsMappingContextActive` 一起用。 |
| E2 | **GetCurrentInputActionState(ia)** | 小 | 高 | PIE 时读 `UEnhancedPlayerInput::ActionData` —— 当前 ETriggerEvent / FInputActionValue / ElapsedTriggeredTime。Agent 看"我刚 Inject 的输入到没到"用。 |
| E3 | **TraceInputEvents(seconds, ia_filter?)** | 中 | 中-高 | 注册临时 ETriggerEvent listener，把 N 秒内所有 IA 触发记到 ring，末了一次性返回 `[{t, ia, event, value}]` 时序。回归测试 / 录制 demo 必备。 |
| E4 | **DumpStickyInputs() + DumpInjectedQueue()** | 小 | 中 | Agent 自己排查"为什么 Pawn 一直在动"——列出 bridge 内部 sticky 表 + EnhancedInput pending injects。 |
| E5 | **SimulateKeyEvent(key, pressed, gamepad_id?)** | 小-中 | 中 | 走 `FSlateApplication::ProcessKeyDownEvent` 注入"真键盘事件"，覆盖**未走 IA**的逻辑（菜单 / Slate / 旧 InputComponent）。`TriggerInputAction` 不能解决这块。 |

### F. 旧版 InputAxis / ActionMappings（Project Settings .ini）

| # | 项目 | 工程量 | 频次 | 说明 |
|---|---|---|---|---|
| F1 | **ListInputAxisMappings() / ListInputActionMappings()** | 小 | 中 | 走 `UInputSettings::GetDefault()->GetAxisMappings()` / `GetActionMappings()` —— 5.0 之前的项目核心数据。 |
| F2 | **AddInputAxisMapping(name, key, scale)** + **AddInputActionMapping(name, key, mods)** | 小 | 中 | `UInputSettings::AddAxisMapping` + `SaveKeyMappings`，写 `DefaultInput.ini`。 |
| F3 | **RemoveInputAxisMapping / RemoveInputActionMapping** | 小 | 中 | 对称的删。 |
| F4 | **MigrateLegacyInputToEnhanced(scope)** | 中-大 | 低 | 一键把 `DefaultInput.ini` 的 ActionMapping/AxisMapping 转成 IA 资产 + IMC。结合 C5 一起做才有意义。 |

### G. 模板 / 一键脚手架

| # | 项目 | 工程量 | 频次 | 说明 |
|---|---|---|---|---|
| G1 | **ImportInputBindingsFromJson(json)** | 中 | 高 | 一份 JSON 描述完整输入栈：IA 列表 + 每个 IA 的 ValueType/Triggers/Modifiers + IMC 列表 + IMC 内的 mapping。一次创建/更新整套。Agent 用 LLM 生成 JSON 远比逐个调原语稳定。 |
| G2 | **ExportInputBindingsToJson(scope)** | 小 | 中 | 反向：把现有 IA/IMC 序列化成 G1 同格式，diff / 备份 / 跨项目复制。 |
| G3 | **PresetFirstPersonInput / PresetTopDownInput / PresetThirdPersonInput** | 中 | 中 | G1 的预制 JSON：标准 `IA_Move (Axis2D)` / `IA_Look (Axis2D)` / `IA_Jump (Bool)` / `IA_Fire`，每个标 trigger，IMC 全部默认 WASD/Mouse/SpaceBar/LMB。Agent 起手 5 秒搭好。 |

---

## 3. 优先级 / 实施顺序

P0（解锁"agent 能从零写一个可玩 Pawn"这条主线 —— 直接对应用户反馈）：**全部交付 2026-05-07。**

| 排名 | 项 | 状态 | 解锁能力 |
|---|---|---|---|
| 1 | ~~**B1 AddEnhancedInputActionEventNode**~~ | ✅ commit `302ca97` | `add_enhanced_input_action_event_node(bp, graph, ia_path, x, y)`：5 exec out + ActionValue（按 IA ValueType 强类型化）+ ElapsedSeconds/TriggeredSeconds + InputAction object pin。同 IA 第二次调用复用既有节点。 |
| 2 | ~~**B6 AddGetInputActionValueNode**~~ | ✅ commit `9e347af` | `add_get_input_action_value_node(bp, graph, ia_path, x, y)`：纯 K2Node，输出 ReturnValue 按 IA ValueType 强类型化。 |
| 3 | ~~**B7 WireEnhancedInputActionToFunction**~~ | ✅ commit `0e7047b` | `wire_enhanced_input_action_to_function(bp, graph, ia, trigger, target_class, fn, evX, evY, fnX, fnY)` → `FBridgeWireIAResult{event_node_guid, call_node_guid, wired, failure_reason}`。**B7 redefined**：原 BindAction wrapper 不可行（`UEnhancedInputComponent::BindAction` 不是 BlueprintCallable，BP 编译器 ExpandNode 自动展开 K2Node_EnhancedInputAction→BindAction），改成"事件节点 + CallFunction + 接 trigger exec→execute"组合。 |
| 4 | ~~**A1 CreateInputAction / A2 CreateInputMappingContext**~~ | ✅ commit `96838ea` | `create_input_action(path, "Boolean"/"Axis1D"/"Axis2D"/"Axis3D", desc, save)` + `create_input_mapping_context(path, desc, save)`。直接 CreatePackage + NewObject + AssetRegistry::AssetCreated（EnhancedInput 不带 *Factory 类）。AssetRegistry 立即可见，B1 / B7 立即可吃新建的 IA。 |
| 5 | ~~**A4 AddTriggerToIA / A6 AddModifierToIA**~~（+ A5 对称 Remove） | ✅ commit `4579578` | `add_trigger_to_ia(ia, "Hold"/long-name/full-path, json, save) → idx`，JSON 走 `FJsonObjectConverter::JsonObjectToUStruct` 写 UPROPERTY；同样的 `add_modifier_to_ia`；附 `remove_*_from_ia(ia, idx)` 支持负数下标。 |
| 6 | ~~**C4 ScaffoldEnhancedInputPawn**~~ | ✅ commit `a68c8b3` | Python 辅助 `unreal_bridge_helpers.scaffold_enhanced_input_pawn(bp, ia_action_map, parent_class)`：建 BP（如缺）+ 建 target function graphs + 对每条 (ia → trigger, fn) 调 B7。一次 Python 调用出可编译 Pawn。IMC 应用先留给 caller（runtime AddMappingContext 或 PlayerController 的 DefaultPawnInputMappingContext 字段；BeginPlay graph 自动生成是 P1 follow-up）。 |

P1（补完调试与维护链路）：

| 排名 | 项 | 估时 | 理由 |
|---|---|---|---|
| 7 | **D1 FindIAReferences + C1 FindBindActionCallSites + C2 FindEnhancedInputEventNodes** | 小-中 | "哪儿用到了这个 IA" 类问题 100% 频次，删/改 IA 前必跑 |
| 8 | **A7/A8 per-mapping Trigger/Modifier in IMC** | 中 | 79fb462 out-of-scope #2 收尾 |
| 9 | **E1 GetActiveMappingContextStack + E2 GetCurrentInputActionState** | 小 | PIE 排查 "input 没生效" 的最短路径 |
| 10 | **E3 TraceInputEvents** | 中 | 回归 / soak / golden-image 配套 |

P2（旧版兼容 + 玩家自定义键 + 一键导入）：

| 排名 | 项 | 估时 | 理由 |
|---|---|---|---|
| 11 | **G1 ImportInputBindingsFromJson** | 中 | LLM 友好，能跨项目搬整套 |
| 12 | **A3 SetInputActionProperty + A9 PlayerMappableKeySettings** | 小-中 | 玩家重绑定 UI 数据源 |
| 13 | **F1/F2/F3 旧 Axis/Action Mappings 读写** | 小 | 老项目兼容 |
| 14 | **B2/B3/B4/B5 旧 K2 节点工厂** | 小 | 老项目兼容 |
| 15 | **D3 ValidateInputBindings + D4 DetectKeyConflicts** | 中 | lint，cleanup 前跑 |
| 16 | **E5 SimulateKeyEvent** | 小-中 | 给非-IA 的 Slate / 旧 InputComponent 路径补一刀 |

P3（大件，单独立项再做）：

| 项 | 理由 |
|---|---|
| C5 ConvertLegacyInputBindingToEnhanced | 大改图，使用面窄；在 P2 的 F1-F4 + B1-B7 都齐之后再说 |
| F4 MigrateLegacyInputToEnhanced | 同上 |
| G3 Preset* | G1 落地后用 JSON 预制即可，不需要 C++ |

---

## 4. 排除项（不打算做）

- **运行时玩家重绑定 UI** —— 这是 UMG 题目，不是 input bridge 题目；提供 A9 的写口
  即可，UI 由项目自己拼 / LLM 生成。
- **手柄震动 / Force Feedback** —— 已是 GameplayLibrary 其他节的范畴。
- **Common UI / EnhancedInputUserSettings** —— 5.5+ 的玩家自定义键存档框架，
  专题大，等有具体需求再立项。
- **GameplayTags 触发器（InputTriggerComboAction、InputTriggerChordAction 链路图化）** ——
  A4 的 trigger 实例化已经覆盖；图化是 GAS-graph-editing 那条线的活。

---

## 5. 实施时的非显然坑（写到这里防遗忘）

1. **K2Node 写入序列**：必须 `Object 构造 → 设绑定字段 → AddNode → PostPlacedNewNode →
   AllocateDefaultPins → ReconstructNode → MarkBlueprintAsStructurallyModified`。
   漏 `ReconstructNode` → pin 显示对但内部 `bAllocateDefaultPinsCalled` 状态错，
   compile 时静默丢节点。
2. **`UInputAction::ValueType` 改了之后**所有引用它的 IMC mapping 的 trigger
   stack 需要 `PostEditChangeProperty` 才会刷新；否则保存的 .uasset 与运行行为不一致。
3. **PlayerMappableKeySettings 5.3 vs 5.4+ 类不同**：5.3 是 `FPlayerMappableKeyOptions`
   struct（per-mapping 字段），5.4+ 是 `UPlayerMappableKeySettings*` 资产引用。
   A9 实现要 `#if UE_VERSION_OLDER_THAN(5,4,0)` 分支（注意：根据 CLAUDE.md 与 memory，
   main 分支只跟 5.7；这条留作 legacy 分支再改的备忘）。
4. **`AddMappingContext` 必须在 LocalPlayer Subsystem 已经有 `PlayerInput` 后调用**——
   PossessedBy 比 BeginPlay 早，但 Subsystem.AddMappingContext 内部要求 PlayerController
   的 PlayerInput 已建好。C4 脚手架挂在 BeginPlay 是稳的，挂在 PossessedBy 偶发 nullptr。
5. **`UEnhancedInputComponent::BindAction` 的 FunctionName 写错不会编译报错**——
   运行时静默不触发。C1/B7 必须在写入时验证 target 类有这个 UFUNCTION 且签名兼容
   `(const FInputActionInstance&)`，否则报 ValidationError 而不是写完就走。
6. **`K2Node_EnhancedInputAction` 的 TriggerEvent 不是节点字段**——5 个 exec pin 各代表
   一个 ETriggerEvent；节点本身只绑 IA。这跟 `BindAction(IA, ETriggerEvent::X, ...)`
   不一样，B7 文档要明确区分。
7. **per-mapping trigger 数组（A7）实际写在 `FEnhancedActionKeyMapping::Triggers`**——
   不在 IMC 顶层；IMC 顶层 `DefaultKeyMappings.Mappings[i]` 才是写入点。5.7+ 用
   `GetMapping(i).Triggers.Add(...)`，5.3-5.6 用 `Mappings[i].Triggers.Add(...)`。
   `GetInputMappingContextMappings` 已经处理了这个版本差，A7/A8 跟着同一套版本宏即可。
8. **资产创建（A1/A2）走 `FAssetToolsModule::Get().CreateAsset`**，**不要**直接
   `NewObject` + `Package`——前者帮你做 ContentBrowser 通知 + redirector + ULevel
   bookkeeping，后者写出来的文件 ContentBrowser 经常显示但打不开。

---

## 相关历史
- `docs/plans/blueprint-capability-roadmap.md` —— P0 #4 "Enhanced Input 绑定" 应改写为
  "见 `enhanced-input-roadmap.md`" 以避免重复。
- 2026-05-06 commit `79fb462`（Enhanced Input authoring 第一波）—— 本文档承接的起点。
- `.claude/memory/retrievable/feedback_locomotion_strafe_and_look.md` —— IA_Move 是
  camera-relative 这条先验对 C4 PresetFirstPerson 的默认 modifier 链（`SwizzleAxis`）
  设计直接相关。
