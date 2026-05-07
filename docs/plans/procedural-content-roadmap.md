# Procedural Content 拓展路线图（不依赖 PCG Graph）

> 目标：把 bridge 在程序化内容生成（scatter / mesh authoring / 实例放置）这一块从"零"
> 推到"agent 写 Python + 调 C++ 原语就能搭场景、造几何"。
>
> 设计前提：**agent 写代码 / 脚本强，写 visual graph 弱**（参考 2026-05-07 与用户的
> 设计讨论）。本路线图把 procedural 能力建在"采样函数 + 过滤函数 + 实例化函数 +
> mesh 操作函数"这层 —— UE 自带的 line trace / Landscape / ISM/HISM / Geometry Script
> 是底座，bridge 做 thin wrap + 批处理 + JSON 友好化 + seed 化。**不**引入 PCG 图编辑。
>
> 写于 2026-05-07。已交付的 `feat(perf)` / `feat(input)` 系列后的代码状态。
>
> 范围与 `agent-capability-gaps.md` A9-#35 (Scatter / grid 布景) + A9-#36 (Snap-to-surface)
> 对接 —— 本路线图是这两个条目的展开。

---

## 1. 现状基线（2026-05-07）

procedural 类能力本身**零覆盖**（grep `PCG|Procedural|Geometry[Ss]cript|InstancedStaticMesh`
只命中 1 处材质 ISM 引用）。但**下层原语**已就绪：

`UnrealBridgeLevelLibrary` 已有：

- `LineTraceFirstActor` / `LineTraceHitInfo` / `MultiLineTraceActors` /
  `SphereTraceFirstActor` / `MultiSphereTraceActors` / `BoxTraceFirstActor`
  —— 全部走 **runtime world**（PIE 优先），visibility channel + `bTraceComplex=true`。
- `GetHeightAt(X, Y, ZStart, ZEnd)` —— 单点向下 line-trace；本质就是 surface-sample 的
  最小原子（`UnrealBridgeLevelLibrary.cpp:1571`）。
- `GetHeightProfileAlong(StartXY, EndXY, SampleCount)` —— 沿线段批 sample。
- `ProbeFanXY` —— 单点放射 N 条射线。
- `FindActorsInRadius` / `GetActorsInBox` —— 已有的空间查询。
- `SpawnActor` —— 单 actor，走 `FScopedTransaction` + `ESpawnActorCollisionHandlingMethod::AlwaysSpawn`。

也就是说"采样"原语其实已经写过一次了 —— 只是限定在 line-trace 用例。本路线图把它扩成
通用的 procedural 采样族。

模块依赖现状（`Build.cs`）：`Engine` / `NavigationSystem` 已链接；**未链接**
`GeometryScriptingCore` / `Foliage`。`.uplugin` 没声明 `GeometryScripting` / `PCG`。

UE 5.7 的 Geometry Script 在 `G:\UnrealEngine\UE_5.7\Engine\Plugins\Runtime\GeometryScripting\`，
40+ 个 Function library，全部是 `UBlueprintFunctionLibrary` 静态 UFUNCTION，wrap 起来
1:1 直转。

---

## 2. 设计决策

### D1. 三库分立

| 库 | 内容 | 依赖 |
|---|---|---|
| `UnrealBridgeProceduralLibrary` | Lane 1 — sampling / filter / instance | 仅 `Engine` + `NavigationSystem`（已有），无新依赖 |
| `UnrealBridgeGeometryLibrary`   | Lane 2 — Geometry Script wrap        | 加 `GeometryScriptingCore` + `GeometryFramework`，`.uplugin` 加 `GeometryScripting` |
| (Lane 3 库, 推后)               | PCG 只读 + 触发                       | 加 `PCG` 模块 + `.uplugin` 加 `PCG` plugin |

不混合。Lane 1 几乎无外部依赖，5.3-5.7 全版本通过；Lane 2 依赖 plugin + Geometry Script
API 在 5.7 才稳定，整库 gate；Lane 3 单独立项。失败模式与版本兼容矩阵差异大，硬塞一起
不划算。

### D2. 函数签名约定

所有 sampling / filter 类函数遵循**同一套形状**（点列 in / 点列 out）：

```cpp
TArray<FVector>    SamplePointsXxx(/* 几何参数 */, int32 Count, int32 Seed);
TArray<FVector>    FilterPointsByXxx(const TArray<FVector>& In, /* 阈值 */);
TArray<int32>      AddInstancesByTransforms(const FString& ActorLabel, const TArray<FTransform>& Xs, bool bWorldSpace);
```

理由：
- 点列在 Python 一侧就是 `list[unreal.Vector]`，agent 串管道几乎无心智成本。
- 函数纯 in→out，没有内部状态，**seed 决定结果**（D5 详）。
- 不把 filter 绑死到一个 generation context（这点和 PCG 的 pipeline 模型反向，但更
  适合 Python）。

### D3. 实例化分两步

Lane 1 的实例化层暴露**两阶段**接口而不是"一把梭"：

1. `EnsureProceduralISMActor(Tag, MeshPath, bUseHISM=true) -> ActorName` ——
   找/造一个挂着 ISM/HISM 的 stub Actor（class 用 `AActor` + 手动加
   `UInstancedStaticMeshComponent`，**不**用 `AStaticMeshActor`），返回 label。
   Tag 唯一表示一组 instance。
2. `AddInstancesByTransforms(ActorName, Transforms[], bWorldSpace) -> int32[]` ——
   一把走 `AddInstances(... bShouldReturnIndices=true)`，**不**循环 `AddInstance`
   （N² 重建 BVH）。

Agent 自己决定哪个 actor 装哪类 mesh，bridge 不替它做 grouping。

### D4. World 选择：editor world，**不**走 PIE

与 trace 族**反向** —— trace 是 runtime world（PIE 优先），但 procedural placement
默认落在 **editor world**：

- 这是 level dressing 工作流，PIE 退出后产物要保留；落 PIE world 退 PIE 就丢。
- ISM 的 component 必须在 editor world 上创建，才能进 .umap 序列化。
- 提供可选 `bDuringPie=true` 参数走 PIE world，给"PIE 内动态 spawn 草丛"留路径。
- 文档每个 spawn 函数 docblock 显式说明 world 选择。

### D5. Seed 必须显式

所有采样函数接 `int32 Seed` 必填，内部用 **`FRandomStream`** —— **不**用 `FMath::Rand` /
`FMath::FRand`，那俩共享全局状态，agent "重跑得同结果" 的假设会破。

```cpp
TArray<FVector> SamplePointsPoissonDisk2D(const FBox& Bounds, float MinRadius, int32 MaxAttempts, int32 Seed)
{
    FRandomStream Rng(Seed);  // ← 必须，每次调用 entry 构造
    ...
}
```

每个采样函数 docblock 显式声明 "deterministic given (params, seed)"。

### D6. GameThread 协议

所有 op 跑在 GT（bridge 现状），**不**起 worker：

- 采样 ≤ 1k 点 ≤ ~10ms，同步可接受；
- 1k-100k 点用户应用 split exec —— `sample → return → filter (next exec) → return → spawn (next exec)`；
  受 `feedback_bridge_exec_holds_gamethread.md` 约束。
- 大于 100k 点的场景属于 PCG 真本职，本路线图不覆盖。

每个采样 / 过滤函数 docblock 写明上限，避免 agent 下意识发 1M poisson 卡死 editor。

### D7. 版本策略

跟随 `project_build_matrix.md`（main = 5.7 only）：

- Lane 1 全部 5.3-5.7 兼容（line trace / ISM 都是稳定 API），不打 `UE_VERSION_OLDER_THAN`。
- Lane 2 整库 `#if !UE_VERSION_OLDER_THAN(5, 7, 0)` gate，stub `_Stubs.cpp` 返回
  "geometry script not supported on this engine version"（同 Chooser/PoseSearch 模式）。
  Geometry Script 在 5.3-5.6 也能用，但 5.7 版 API 才定型，不值得回 lower 版本。

---

## 3. Lane 1 — `UnrealBridgeProceduralLibrary`（采样 + 过滤 + 实例化）

总规模：~25 UFUNCTION，~1500 行。无新依赖，5.3-5.7 全版本。

### M1 — 采样原语（~10 ops，~600 行）

| # | UFUNCTION | 入参 | 出参 | 说明 |
|---|---|---|---|---|
| M1-1 | `SamplePointsGrid` | `Bounds: FBox, Spacing: float, JitterRatio: float, Seed: int32` | `TArray<FVector>` | 规则网格 + 抖动；JitterRatio ∈ [0, 0.5]，0 = 严格栅格 |
| M1-2 | `SamplePointsPoissonDisk2D` | `Bounds: FBox (Z 忽略), MinRadius: float, MaxAttempts: int32 (默认 30), Seed: int32` | `TArray<FVector>` (Z=Bounds.Min.Z) | Bridson 算法在 XY 平面采，Z 留给 surface-snap (M2-7) 二次处理；返回点数不可控 |
| M1-3 | `SamplePointsPoissonDisk3D` | `Bounds: FBox, MinRadius, MaxAttempts, Seed` | `TArray<FVector>` | 体积撒（粒子云、岩石阵） |
| M1-4 | `SamplePointsOnSurface` | `ActorLabel: FString, Count: int32, Seed: int32, MaxBounceUp: float (默认 5000)` | `TArray<FVector>` | 取 actor bounds → XY 均匀随机 → 从 (X, Y, Z+MaxBounceUp) 向下 line-trace（visibility, complex collision，与 `GetHeightAt` 同 channel）；命中即 push |
| M1-5 | `SamplePointsOnLandscape` | `LandscapeLabel: FString, Bounds2D: FBox, Count, Seed` | `TArray<FVector>` | 走 `ALandscapeProxy::GetHeightAtLocation`（O(1)，比 surface 版快 5-10x，命中率 100%）；editor-only —— PIE 走 M1-4 |
| M1-6 | `SamplePointsOnSpline` | `SplineActorLabel, ComponentName, Mode: FString ("by_count"\|"by_distance"), CountOrSpacing: float` | `TArray<FVector>` | `USplineComponent::GetLocationAtDistanceAlongSpline` |
| M1-7 | `SamplePointsInVolume` | `VolumeActorLabel, Count, Seed, MaxAttempts` | `TArray<FVector>` | bounds 内 reject sample，rejection test 用 `OverlapBlockingTestByObjectType` |
| M1-8 | `SamplePointsJitterStratified` | `Bounds: FBox, GridResolution: int32, Seed` | `TArray<FVector>` | 每个 cell 一个点 + 抖动；poisson 廉价替代，O(N) |
| M1-9 | `SampleTransformsAlongSpline` | `SplineActorLabel, ComponentName, Mode, CountOrSpacing` | `TArray<FTransform>` | M1-6 + 切线→rotation；路灯/护栏摆放标配 |
| M1-10 | `JitterTransforms` | `Xs: TArray<FTransform>, PosSigma, RotSigma (deg), ScaleMin, ScaleMax: FVector, Seed` | `TArray<FTransform>` | 后处理：position gauss / rotation gauss（欧拉，°）/ scale uniform；一次 stream 确定性 |

输出 USTRUCT 不另起 —— 直接 `TArray<FVector>` / `TArray<FTransform>` 已经是 Blueprint
原生，Python 端就是 `list[unreal.Vector|Transform]`。

### M2 — 过滤原语（~7 ops，~400 行）

| # | UFUNCTION | 说明 |
|---|---|---|
| M2-1 | `FilterPointsBySlope(Pts, MaxSlopeDeg, BounceUp=5000)` | 每点向下 trace 取法线，与 +Z 夹角 > MaxSlope 的丢弃 |
| M2-2 | `FilterPointsByOverlap(Pts, BlockingClassPaths: TArray<FString>, Radius)` | 每点 sphere overlap test，命中给定 actor classes 即丢弃；用于"避开建筑/水域" |
| M2-3 | `FilterPointsByMinDistance(Pts, MinDist)` | post-poisson 二次稀疏化；用 grid bucket（avoid 第三方 KD-tree） |
| M2-4 | `FilterPointsByDensityMask(Pts, TextureAssetPath, BoundsXY: FBox, ChannelIndex=0, Threshold=0.5, Seed)` | BoundsXY → texture UV，sample R/G/B/A，stochastic threshold；texture 需 `TC_VectorDisplacementmap`（坑 #11） |
| M2-5 | `FilterPointsInsideActor(Pts, ContainerActorLabel, bInside=true)` | 用 actor bounds 或 collision shape，true 保留内部、false 保留外部 |
| M2-6 | `FilterPointsByLandscapeLayer(Pts, LandscapeLabel, LayerName, Threshold=0.5)` | 按 landscape weightmap 层 strength 过滤 |
| M2-7 | `ProjectPointsToSurface(Pts, BounceUp=5000, BounceDown=5000, OutHitNormals: TArray<FVector>&)` | 垂直对齐到表面 + 输出法线（M3 实例化对齐用） |

### M3 — 实例化原语（~6 ops，~500 行）

| # | UFUNCTION | 说明 |
|---|---|---|
| M3-1 | `EnsureProceduralISMActor(Tag, MeshPath, bUseHISM=true) -> ActorName` | 找有 tag 的 stub actor，没有就 `World->SpawnActor<AActor>` + `NewObject<U[H]ISMC>` + `Mesh->Set` + `RegisterComponent` + `Actor->AddInstanceComponent`，挂上 tag |
| M3-2 | `AddInstancesByTransforms(ActorName, Xs, bWorldSpace=true) -> TArray<int32>` | `[H]ISMC->AddInstances(Xs, /*bShouldReturnIndices*/true, bWorldSpace, /*bUpdateNavigation*/false)`；nav 后置（M3-6） |
| M3-3 | `RemoveInstancesByIds(ActorName, InstanceIds[])` | indices 去重 + 倒序排再 `RemoveInstances(SortedIds, /*bAlreadySortedReverse*/true)` |
| M3-4 | `UpdateInstanceTransformsByIds(ActorName, Ids, NewXs, bWorldSpace=true)` | `BatchUpdateInstancesTransforms`，一调一刷 |
| M3-5 | `ClearInstances(ActorName)` | `[H]ISMC->ClearInstances()` + `MarkRenderStateDirty` |
| M3-6 | `RebuildProceduralNavigation(ActorName)` | M3-2 跳过 nav update，最后调一次 `BuildTreeIfOutdated` + nav system rebuild |

### Lane 1 联合验收

Agent 一次 conversation 内能完成："在选中 landscape 上撒 3000 个树点位，坡度 < 30°，
彼此距离 > 3m，按 density mask 调密度，jitter rotation/scale，HISM 化"——总计 ~7 个
bridge 调用串成一条链，无图编辑：

```python
# 伪代码示意
pts  = ub.sample_points_on_landscape("Landscape_0", bounds, 3000, seed=42)
pts  = ub.filter_points_by_slope(pts, 30.0)
pts  = ub.filter_points_by_min_distance(pts, 300.0)
pts  = ub.filter_points_by_density_mask(pts, "/Game/Masks/T_Forest", bounds, 0, 0.4, seed=42)
xs, normals = ub.project_points_to_surface(pts, 5000, 5000)  # M2-7
xs   = ub.jitter_transforms([as_transform(p) for p in xs], 50.0, 15.0, [0.8,0.8,0.8], [1.2,1.2,1.2], seed=42)
actor = ub.ensure_procedural_ism_actor("Trees_PineForest", "/Game/Trees/SM_Pine", True)
ids   = ub.add_instances_by_transforms(actor, xs, True)
ub.rebuild_procedural_navigation(actor)
```

---

## 4. Lane 2 — `UnrealBridgeGeometryLibrary`（Geometry Script wrap）

整库 `#if !UE_VERSION_OLDER_THAN(5, 7, 0)` gate；`.uplugin` 加 `GeometryScripting` 依赖；
`Build.cs` 加 `GeometryScriptingCore`、`GeometryFramework`；stub `_Stubs.cpp` 走整库 stub
模式（同 Chooser）。总规模：~23 UFUNCTION，~1100 行。

### M4 — Mesh handle 管理 + 资产读写（~8 ops）

`UDynamicMesh` 是 Geometry Script 的核心数据结构。bridge 用一张 process-global 的
`TMap<int32, TStrongObjectPtr<UDynamicMesh>>` 持有，handle 是 int —— 与 NavGraph 的
process-global 单例同源（参考 `UnrealBridgeLevelLibrary.h:618 NavGraph` 注释）。

| # | UFUNCTION | 说明 |
|---|---|---|
| M4-1 | `CreateDynamicMesh() -> int32 handle` | 新建 transient `UDynamicMesh`，记入 map，返回 handle |
| M4-2 | `ReleaseDynamicMesh(handle)` | 从 map 删；`TStrongObjectPtr` 析构后 GC 回收 |
| M4-3 | `LoadMeshFromStaticMesh(handle, AssetPath, Lod=0) -> bool` | 包 `UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMesh`；outcome enum 转 bool |
| M4-4 | `SaveMeshToNewStaticMesh(handle, NewAssetPath, MaterialList: TArray<UMaterialInterface*>) -> FString` | 包 `CreateNewStaticMeshAssetFromMesh`；优先走 `FAssetToolsModule::CreateAsset`（StaticMesh 有 UFactory），仅当目标类型无 UFactory 子类时才退回 `CreatePackage`+`NewObject`+`FAssetRegistryModule::AssetCreated` 套路（参 `UnrealBridgeGameplayLibrary::CreateInputAction`）。 |
| M4-5 | `SaveMeshToExistingStaticMesh(handle, ExistingAssetPath, bReplaceMaterials)` | 包 `CopyMeshToStaticMesh`；必须 `Modify()` + `MarkPackageDirty()` |
| M4-6 | `GetMeshInfo(handle) -> FBridgeMeshInfo {tri, vert, uv_layers, has_normals, bounds, has_vertex_colors}` | `MeshQueryFunctions`；agent 操作前后 sanity check |
| M4-7 | `ListDynamicMeshHandles() -> TArray<int32>` | 调试，避免 handle 泄漏 |
| M4-8 | `LoadMeshFromComponent(ActorLabel, ComponentName, handle) -> bool` | 包 `CopyMeshFromComponent`；从关卡 actor 直接抓 mesh |

USTRUCT：

```cpp
USTRUCT(BlueprintType)
struct FBridgeMeshInfo
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry") int32 NumTriangles = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry") int32 NumVertices = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry") int32 NumUVLayers = 0;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry") bool  bHasNormals = false;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry") bool  bHasVertexColors = false;
    UPROPERTY(BlueprintReadOnly, Category = "UnrealBridge|Geometry") FBridgeActorBounds Bounds;  // 复用现有 Bounds USTRUCT
};
```

### M5 — Mesh 几何操作（~12 ops）

只 wrap 最常用的，**不**全量映射 Geometry Script 的几百个函数（agent 直接 `unreal.<Lib>.func()`
更合算）。bridge 这层只挑"参数复杂、带 enum/struct、容易写错"的封装：

| # | UFUNCTION | 来源 lib | 说明 |
|---|---|---|---|
| M5-1 | `AppendBox(handle, Origin, Size)` / `AppendSphere(handle, Origin, Radius, ResolutionUV)` / `AppendCylinder(handle, ...)` / `AppendCone(handle, ...)` | `MeshPrimitiveFunctions` | 4 个原语合一 |
| M5-2 | `MeshBoolean(handleA, handleB, Op: FString)` Op ∈ "union"/"intersect"/"subtract" | `MeshBooleanFunctions::ApplyMeshBoolean` | 字符串 → `EGeometryScriptBooleanOperation` |
| M5-3 | `MeshDisplaceFromTexture(handle, TextureAsset, Magnitude, UVChannel)` | `MeshDeformFunctions` | heightmap displace |
| M5-4 | `MeshSmooth(handle, Iterations, Strength)` | `MeshDeformFunctions` | uniform smoothing |
| M5-5 | `MeshDecimate(handle, TargetTris)` | `MeshSimplifyFunctions` | tri-count decimation |
| M5-6 | `MeshVoxelMerge(handles[], CellSize)` | `MeshVoxelFunctions::ApplyMeshSolidify` | 多 mesh → watertight |
| M5-7 | `MeshUVUnwrap(handle, Method: FString)` Method ∈ "auto"/"box"/"cylinder"/"plane" | `MeshUVFunctions` | UV 自动展开 |
| M5-8 | `MeshTransform(handle, Transform: FTransform)` | `MeshTransformFunctions` | apply transform |
| M5-9 | `MeshTriangulate(handle, MinAngleDeg)` | `MeshRemeshFunctions` | 网格化清理 |
| M5-10 | `MeshSelectionByPlane(handle, Origin, Normal) -> int32 SelectionId` | `MeshSelectionFunctions` | 留 SelectionId 给 M5-11 |
| M5-11 | `MeshExtrudeSelection(handle, SelectionId, Distance)` | `MeshModelingFunctions` | bridge 内 selection map 持有 selection 句柄 |
| M5-12 | `MeshSweepAlongSpline(handle, ProfilePoints: TArray<FVector2D>, SplineActorLabel, ComponentName)` | `MeshPrimitiveFunctions` | 沿 spline 扫掠（路、栏杆） |

### M6 — Bake / 收尾（~3 ops）

| # | UFUNCTION | 说明 |
|---|---|---|
| M6-1 | `BakeNormalsFromMeshToTexture(SourceHandle, TextureAssetPath, Resolution)` | `MeshBakeFunctions::BakeNormalsToTexture`，输出新 texture asset |
| M6-2 | `BakeOcclusionToVertexColor(handle, Iterations)` | `MeshBakeFunctions::BakeAmbientOcclusionToVertexColors` |
| M6-3 | `RecomputeNormalsAndTangents(handle, AngleThreshold)` | 跨 boolean/bake 后必跑 |

### Lane 2 联合验收

Agent："读 SM_Block，相对它做 union 一个 sphere，smooth 2 步，UV unwrap，存为
SM_Block_Modified" —— 6 个 bridge 调用，无图编辑。

---

## 5. Lane 3（推后 / 占位）— PCG 只读 + 触发

**不做**完整 PCG graph editing。只在用户工程已经有人手写 PCG 资产时，给 agent 一个
"看 + 跑"的能力。规模 ≤ 8 个 UFUNCTION，单 commit 落地，**Lane 1 / Lane 2 落地后才考虑**。

| # | UFUNCTION | 说明 |
|---|---|---|
| L3-1 | `ListPCGGraphAssets(Filter, Max)` | AssetRegistry 按 `UPCGGraph` class 过滤 |
| L3-2 | `ListPCGComponentsInLevel(LevelFilter, Max)` | 关卡内 `UPCGComponent` 枚举 + 状态 |
| L3-3 | `GetPCGComponentState(ActorLabel, ComponentName)` → `{graph, generated, dirty, last_gen_iso, generated_bounds}` | 单组件状态 |
| L3-4 | `GetPCGComponentOverrides(ActorLabel, ComponentName)` → `TArray<FBridgePCGOverrideEntry>` | 已 set 的参数覆盖 |
| L3-5 | `SetPCGComponentOverride(ActorLabel, ComponentName, Name, ExportedValue: FString)` | `Property->ImportText`，agent 改参数 |
| L3-6 | `TriggerPCGGenerate(ActorLabel, ComponentName, bForce=false)` | `UPCGComponent::Generate(bForce)` —— 异步立即返回 |
| L3-7 | `WaitForPCGGenerate(ActorLabel, ComponentName, TimeoutSec=60)` → `{success, elapsed_ms, output_summary}` | 轮询 `IsGenerating` + 超时（同 hot_reload.py 模式） |
| L3-8 | `CleanupPCGComponent(ActorLabel, ComponentName)` | 包 `Cleanup` |

依赖：`.uplugin` 加 `PCG`；`Build.cs` 加 `PCG` 模块；整库 5.7-only gate。

**Lane 3 不在主推路径**。先看 Lane 1 / Lane 2 落地后用户是否真有"读现成 PCG 工程"
需求。本路线图也不展开 PCG 写图能力 —— 即使将来要做，也走"`UPCGBlueprintElement`
脚本节点"模式（agent 写 BP element 的 execute 逻辑、不写图拓扑），而不是 graph node
add/wire 这条 PCG 内部 API 路径。

---

## 6. 实施时的非显然坑

1. **ISM Actor 不能用 `AStaticMeshActor`** —— 它的 `RootComponent` 是
   `UStaticMeshComponent` 而非 ISM。要 `World->SpawnActor<AActor>` + `NewObject<U[H]ISMC>`
   + `RegisterComponent` + `Actor->AddInstanceComponent`（参考现有
   `UnrealBridgeLevelLibrary.cpp:4221` 已有 `AddInstanceComponent` 调用）。漏
   `AddInstanceComponent` 不进 editor details。
2. **HISM 的 `BuildTreeIfOutdated` 是 nav rebuild 的 prerequisite** —— 不调它，下次 AI
   pathing 仍走老 nav。M3-6 必须显式触发；M3-2 默认 `bUpdateNavigation=false` 规避 N
   次重建，由 agent 在批末尾调一次 M3-6。
3. **`AddInstances` 拿 indices 必须 `bShouldReturnIndices=true`** —— Engine 默认 false（性能），
   要显式 true 才能拿 ID 给后续 update/remove 用。HISM 与 ISM 同此约定。
4. **PoissonDisk2D 在 Bounds 退化（min == max）时**返回空数组而非死循环 —— Bridson 的
   background grid cell size = R/√2，bound size 0 → 0 cell → 空。要早 return。
5. **Landscape 高度采样的两套 API**：
   - `ALandscapeProxy::GetHeightAtLocation` —— editor-only，O(1)，但跨 LandscapeStreamingProxy
     的边界点会返回 `false`；agent 大区域 sample 必须遍历**所有** proxy fallback 一次。
   - `LineTraceSingleByChannel` —— PIE 也能用，但 ~5x 慢，且 landscape collision LOD 可能比
     可视 mesh 粗（命中位置偏差几十 cm）。
   - bridge 的 `SamplePointsOnLandscape`（M1-5）默认走前者，文档显式说"PIE 内不支持"，
     agent 想 PIE 内 sample 用 `SamplePointsOnSurface`（M1-4）。
6. **`FRandomStream` 必须在函数 entry 构造**，不能 `static FRandomStream` 缓存 ——
   reactive handler 跨线程调用会 race；每次新建消除该风险，构造廉价。
7. **`UDynamicMesh` 的生命周期** —— Lane 2 的 handle map 必须用 `TStrongObjectPtr` 或
   `UPROPERTY()` 容器持有，否则 GC 一过 mesh 就死。参考 `UnrealBridgeReactiveSubsystem`
   持有 `TArray<TStrongObjectPtr<UObject>>` 的模式。
8. **GeometryScript 的 `meta=(ScriptMethod)`** 让 Python 里 first-param-as-self；bridge
   wrap 不暴露 dynamic mesh handle 当 self（int 不能是 self），所以全部走 explicit
   param。从 bridge 看出去就是普通静态 UFUNCTION。
9. **`CreateNewStaticMeshAssetFromMesh` 必须在 editor world 调** —— 它内部走
   `AssetTools::CreateAsset` + ContentBrowser 通知；非 editor world（PIE / cmdlet）会
   silently skip。Lane 2 整库 `WITH_EDITOR` gate 兜一下。
10. **`InstancedFoliage` vs `ISM`/`HISM` 取舍** —— 项目里如果用 Foliage 工具刷过的草/树，
    对应是 `AInstancedFoliageActor` + `UFoliageInstancedStaticMeshComponent`，不是普通
    HISM。M3 默认创建独立 stub actor + HISM，避免污染 Foliage 工具的资产；后续若需要
    "接管 Foliage" 再加新 op。
11. **Density mask texture 必须 `CompressionSettings = TC_VectorDisplacementmap`** 才能
    runtime 读 raw color；默认压缩 `RuntimeBulkData` 不让你拿。M2-4 文档教 agent 改压缩
    设置，或 bridge 这层加 `EnsureTextureReadable(asset_path)` helper（先做 M2-4，必要再加）。
12. **采样函数返回为空 vs 失败的区分** —— `SamplePointsOnSurface(actor=空)` 和
    `SamplePointsOnSurface(actor=有，但全部 line trace miss)` 都返回空数组。bridge 用
    out 参数 `OutDiagnostics: FString` 透传"actor not found" / "0 hits / 1000 attempts"
    这种诊断；空数组不是 error code。文档统一规约。
13. **`UE Python 把 `bShould*` / `bIs*` 前缀剥成 `should_*` / `is_*`**（参考
    `feedback_ue_python_bool_prefix.md`）—— Lane 1/2 所有 USTRUCT 字段命名跟随这条，
    docs 写明 Pythonized 名字。
14. **`feedback_buildplugin_rocket_strict.md`** —— Lane 1/2 所有 UPROPERTY / UFUNCTION
    必须显式 `Category=`，否则 BuildPlugin -Rocket 上 UHT 报错；走 `tools/add_categories.py`
    校验。
15. **`feedback_split_asset_ops.md`** —— Lane 2 的 mesh-to-asset 操作（M4-4 / M4-5）按
    这条 feedback 必须独立 exec 调，文档要写明：`exec1: load + ops` / `exec2: save_to_new`。
    一个 exec 内 create_asset + 多 mesh op 会触发 asset-ref-completing 模态死锁。

---

## 7. 优先级 / 实施顺序

### P0 —— 解锁 "agent 一条 chain 撒树"

| 排名 | 项 | 估时 | 解锁 |
|---|---|---|---|
| 1 | M1-1 / M1-2 / M1-4 / M1-5 SamplePointsGrid + Poisson2D + Surface + Landscape | 2-3 d | 4 个采样原语 = 80% scatter 用例 |
| 2 | M2-1 / M2-3 / M2-7 FilterBySlope + MinDistance + ProjectToSurface | 1-2 d | "坡度 < 30° + 间距 > 3m + 法线对齐" 标配链 |
| 3 | M3-1 / M3-2 / M3-5 / M3-6 EnsureISMActor + AddInstances + Clear + RebuildNav | 2 d | 关闭 Lane 1 端到端 |

P0 验收 = "agent 一条 chain 在 landscape 上撒 1000 个有自然分布的树"。

### P1 —— 完成 Lane 1

M1 / M2 剩余项 + M3-3 / M3-4。补全 spline / volume / density mask / instance 改写。

### P2 —— Lane 2

先做 M4（资产读写）+ M5 的 boolean / smooth / decimate 三个；之后看用户实际用例再补。

### P3 —— Lane 3（PCG 读）

等真有用户工程触发再做。

---

## 8. 排除项（不做）

- **PCG Graph 编辑** —— 显式拒绝。Agent 写代码强、写图弱，graph 编辑不进 bridge scope。
  即使将来要做，也走 `UPCGBlueprintElement`（脚本节点）路径，不走 graph node add/wire。
- **Houdini Engine 集成** —— 跨进程 + 商业 plugin，bridge 够不着。用户用 Houdini 的话
  Bridge 仅在 `.fbx` 落到项目里之后从 Asset 层介入。
- **Landscape heightmap / weightmap 编辑** —— 独立大题（参考 `agent-capability-gaps.md`
  A9-#37），与本路线图正交。
- **Niagara / VFX 程序化** —— 不同子系统。
- **InstancedFoliage 工具产物的接管** —— 见坑 #10。等 Lane 1 落地后用户有需求再加单独
  helper。
- **Runtime PCG 生成（PIE 或 packaged 内）** —— 即使 Lane 3 落地也只 editor world；runtime
  生成是 PCG plugin 自带能力，bridge 不复制。
- **超大规模（>100k 点）的 procedural** —— PCG 真本职（partitioning / async streaming）。
  Bridge 这层守在 ≤100k 点 / 单关卡 dressing 这个量级。

---

## 9. 相关历史

- 2026-05-07 与用户的设计讨论（这文档的母对话）—— 用户明确"agent 写图弱、写代码强"，
  确立 Lane 1 + Lane 2 路线、Lane 3 推后。
- `docs/plans/agent-capability-gaps.md` A9-#35 / A9-#36 —— 本路线图是这两个条目的展开。
- `feedback_bridge_exec_holds_gamethread.md` —— D6 GameThread 协议直接受其约束。
- `feedback_split_asset_ops.md` —— Lane 2 mesh-to-asset 必须独立 exec 调（坑 #15）。
- `feedback_buildplugin_rocket_strict.md` —— Category= 必填（坑 #14）。
- `feedback_ue_python_bool_prefix.md` —— USTRUCT bool 字段命名约定（坑 #13）。
- `project_build_matrix.md` —— Lane 1 全版本，Lane 2 整库 5.7 gate 跟随主线政策。
- `feedback_no_unnamed_namespace.md` —— Lane 1 / Lane 2 的实现 helper 全部用 named
  namespace `UnrealBridgeProceduralImpl` / `UnrealBridgeGeometryImpl`。
- `feedback_uht_no_if_around_reflection.md` —— Lane 2 整库 gate 走 .h 里 stubs.cpp 模式，
  不在 .h 套 `#if`。

---

## 10. 验证 checklist（每个 lane 落地时跑）

- [ ] `python tools/build_matrix.py --only 5.7` BuildPlugin 通过（Lane 1 还要 5.3-5.7 全过）
- [ ] `python tools/build_matrix.py --rocket` 5.7 BuildPlugin -Rocket 通过（Category= 兜底）
- [ ] `bridge.py exec --stdin <<'EOF' ...` 端到端跑 Lane 1 联合验收 chain
- [ ] `bridge.py exec` 跑 Lane 2 mesh-to-asset 端到端（split 成 2 exec，对应坑 #15）
- [ ] `dump_bridge_signature_registry` 看新增 UFUNCTION 的 Pythonized 名（坑 #13 验证）
- [ ] `tools/audit_tech_debt.py` 看新文件无 LOG_TEMP / TODO 残留
- [ ] `bridge-procedural-api.md` / `bridge-geometry-api.md` 文档与 signature 对齐
