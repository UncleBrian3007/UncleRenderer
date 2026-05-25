# ClusterDAG: Flat-Shaded / Seam-Heavy Mesh Simplification (handoff)

## 한 줄 요약
`Assets/sponza/untitled.gltf`의 `primitive[9] = column_c`가 ClusterDAG 빌드에 실패한다. 근본 원인은 **flat-shaded normal**(position당 normal ~2.9개)로 인해 meshopt가 거의 collapse를 못 하는 것. 해결책은 (1) `meshopt_SimplifyPermissive` 활성화 + (2) 수렴 바닥에서 **multi-child root**로 닫기. 커스텀 wedge reducer나 normal 재익스포트 없이 풀린다.

---

## 1. 증상

```
ClusterDAG Primitive[9]: Level 1 ... reduction failed, reason=predicted_parent_count_exceeded ...
ClusterDAG Primitive[9]: Level 1 made no convergence progress, ... reducedAnyGroup=false
Cluster DAG build skipped for primitive[9]; keeping legacy meshlet path only.
```

- 모든 group이 attribute simplify에 실패 → pass-through → 진전 0 → `made no convergence progress`에서 hard-fail.
- meshopt가 단순화를 **15%밖에 못 한다** (예: 1004 → 852 tris). `target_error = FLT_MAX`(무제한)인데도 멈춤 → 에러 한계가 아니라 **토폴로지 제약**에 막힌 것.

## 2. 근본 원인 — flat-shaded normal

`untitled.gltf`를 직접 분석한 결과 (오프라인 Python, glTF accessor 파싱):

| primitive | material | nrm/pos | uv/pos | wedge/pos | 빌드 |
|---|---|---|---|---|---|
| 7 | column_a | 1.79 | 1.50 | 2.39 | OK(추정) |
| 11 | column_b | 1.68 | 1.29 | 1.88 | OK(추정) |
| **9** | **column_c** | **2.92** | 1.60 | **3.07** | **실패** |
| 대부분(leaf/fabric/vase...) | — | 1.0~1.3 | ~1.0 | ~1.1 | OK |

- **column_c는 position당 평균 ~2.9개의 서로 다른 normal**을 가진다 = 사실상 **faceted(flat-shaded)** 메시. 형제 기둥(column_a/b)은 1.7대로 정상 빌드됨.
- meshopt가 막히는 임계는 대략 wedge/pos ≈ 2 부근으로 보인다 (details 2.21까지는 되고 column_c 2.9는 안 됨).

### 로그로도 교차 확인 가능
빌드 로그의 `SimplifyGroupGeometry` 줄에서 `srcVertices / (scratch positions)` = wedge/position이 그대로 드러난다:
- group 0: `srcVertices=1774`, scratch `positions=605` → **1774/605 = 2.93**
- group 1: 2155/766 = 2.81, group 3: 2671/913 = 2.93 — 전 그룹 일관.

(이 값은 전체 attribute 통합 wedge지만, normal 단독 2.9와 거의 일치 → **normal split이 지배적**이라는 뜻.)

## 3. meshopt 내부 동작 (왜 막히는가)

meshopt 1.1(`MESHOPTIMIZER_VERSION 1010`)의 `simplifier.cpp`는 정점을 position으로 weld하고 per-position **wedge** 루프를 만든다(`buildPositionRemap`). 정점 분류(`classifyVertices`, `VertexKind`):

| wedge 수 | 분류 | collapse 가능? |
|---|---|---|
| 1 | `Kind_Manifold` | 자유 |
| 2 (seam 1개) | `Kind_Seam` | **seam 따라서만** (Seam→Seam) |
| **3+** | **`Kind_Locked`** | **전혀 불가** |

→ **column_c는 대부분 3+ wedge라 `Kind_Locked` = 완전 박제**. 그래서 15%에서 멈춘다.
(`kCanCollapse` 테이블: `Locked` 행 전부 0.)

## 4. 해결책 (다른 agent가 재구현할 것)

### 4-1. (핵심) `meshopt_SimplifyPermissive` 활성화
`SimplifyGroupGeometryWithAttributes`의 `meshopt_simplifyWithAttributes` 호출 options 인자:

```cpp
// before
meshopt_SimplifyErrorAbsolute,
// after
meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive,
```

- Permissive는 `Kind_Seam`/`Kind_Locked`를 **`Kind_Complex`로 승격**한다(`simplifier.cpp` Permissive 블록). Complex는 *"모든 wedge가 타겟으로 함께 이동하는 조건으로"* collapse 가능 → flat-shaded normal seam이 wedge를 데리고 녹는다.
- UV는 attribute 에러 metric(weight)에 이미 들어 있어 **soft하게 보존**된다(가로지르는 collapse는 비용이 높아 후순위).
- 헤더상 `meshopt_SimplifyPermissive`는 **"Experimental"** 표기.

#### ⚠️ 하지 말 것: 하드 `meshopt_SimplifyVertex_Protect`로 UV seam 보호
- 시도했다가 되돌림. meshopt의 Protect는 **position 단위**라(한 wedge라도 protected면 그 위치 전체 collapse 금지), UV seam 위치를 보호하면 **거기 있는 normal-only wedge까지 같이 잠긴다**. column_c에서 ~51% 정점이 보호돼 reduce가 다시 stall했다(564 tris에서 멈춤).
- UV가 LOD에서 늘어지면 하드 락 대신 **UV attribute weight를 올려서**(soft) 대응한다(§6).

### 4-2. (핵심) 수렴 바닥에서 multi-child root로 닫기
레벨 reduction 루프의 no-progress 가드를 hard-fail 대신 break로:

```cpp
if (NextClusters.size() >= CurrentClusters.size() && NextTriangleCount >= CurrentTriangleCount)
{
    // 이번 레벨에 아무것도 reduce 안 됨(순수 pass-through, State.Dag 미변경) = 자연 수렴 바닥.
    // 지금까지의 클러스터를 multi-child root로 닫는다(아래 256 상한으로 검증).
    // 이전 레벨들은 실제 LOD를 제공하고, 마지막 클러스터만 더 못 줄이는 것뿐(경계 lock 압력 등).
    if (!bReducedAnyGroup)
    {
        CLUSTER_DAG_LOG_INFO(PrimitiveIndex, "Level " << Level << " reached convergence floor; closing multi-child root, clusters=" << CurrentClusters.size() << " ...");
        break;
    }
    // 일부는 reduce됐는데 net 진전이 없는 경우는 그대로 실패(orphan 클러스터/그룹 방지).
    CLUSTER_DAG_LOG_WARNING(PrimitiveIndex, "Level " << Level << " made no convergence progress ...");
    return false;
}
```

- `!bReducedAnyGroup`(State.Dag를 안 건드린 순수 stall)일 때만 break → orphan 없음.
- column_c는 이 경로로 **98→46→25→12→root**의 4-레벨 DAG를 만든다. Level 4에서 12개가 **경계 lock ~90%**(open boundary 848개)로 더 못 줄어 12-child root로 닫힌다.

### 4-3. 전제(prerequisite) — baseline에 없으면 같이 구현
아래는 이전 커밋들에 있었으나 revert로 빠졌을 수 있음. 위 두 핵심이 동작하려면 필요:

1. **Multi-child root 지원** (셰이더 변경 불필요 — 검증 완료):
   - `FClusterDAG::IsValid()` (`Source/Scene/ClusterDAG.h`): `RootGroup.ChildRefs.size() == 1` 요구를 제거하고 `bRoot` 유지 + `!empty() && size() <= GClusterDAGMaxRootChildRefs`로.
   - `constexpr uint32_t GClusterDAGMaxRootChildRefs = 256;` 추가. **셰이더 상수 `kClusterDagLevelSplitMaxChildRefsPerGroup`(`Shaders/ClusterDag/ClusterDagCommon.hlsl`, 값 256)와 동기화** — 주석 명시.
   - 빌더 루프 후처리: `CurrentClusters.size() != 1` 실패 조건을 `> GClusterDAGMaxRootChildRefs`로 완화.
   - **검증된 사실**: 두 traversal init 셰이더(`InitClusterDagLevelSplitQueues.hlsl`, `InitClusterDagQueues.hlsl`) 모두 root를 **그룹 인덱스로** 큐에 seed하고, node-cull이 그룹 child를 256까지 처리하며, 런타임 검증(`ClusterDagRuntime.cpp`)은 `ChildRefCount==0`만 거부. 따라서 **traversal/런타임 셰이더 변경 불필요.**
   - node-cull은 256 초과를 조용히 clamp(overflow 카운터만)하므로, 빌더+IsValid의 256 상한이 stale 캐시 누락을 막는 안전장치.

2. **실패 group pass-through** (`SimplifyMergedClusterGroup`): attribute simplify 실패 시 해당 group의 child 클러스터를 그대로 다음 레벨로 넘김(전체 primitive 실패 방지). 단 §4-2의 convergence-floor와 짝을 이뤄야 함.

3. **Parent overshoot 정합** (`SimplifyGroupGeometryWithAttributes`의 성공 게이트): attribute path의 accept 조건을 `DesiredParentCount`가 아니라 **`MaxAllowedParentCount`**로(다운스트림 parent validation과 동일). 안 그러면 통과 가능한 결과를 attribute path가 미리 막음.

### 4-4. 캐시 무효화
`GClusterDAGBuildSemanticVersion`를 올려 `.vmesh` 재생성 강제.

## 5. 진단 로그 추가 (요청사항)

**새 로그 줄을 추가하지 말고, 기존 `SimplifyGroupGeometry` 로그 줄에 wedge/position 필드를 끼워 넣는다.** 이 한 값이 flat-shaded 파편화(column_c ≈ 2.9)를 오프라인 glTF 분석 없이 빌드 로그에서 바로 보여준다.

- `SimplifyGroupGeometry` 로그에는 이미 `srcVertices=`가 있다. unique position 수는 `SimplifyMergedClusterGroup`이 들고 있는 `Scratch.PositionNodes.size()`.
- 권장: `Scratch.PositionNodes.size()`(또는 그 값으로 계산한 비율)를 `SimplifyGroupGeometryWithAttributes`로 넘겨, 기존 INFO/WARNING 로그 줄에 다음을 추가:
  ```cpp
  << ", wedgesPerPosition=" << FormatFloat(static_cast<float>(SourceVertexCount) / static_cast<float>(UniquePositionCount))
  ```
  (이미 `srcVertices`/`positions`가 서로 다른 로그 줄에 흩어져 있으니, 한 줄에 비율로 합치는 게 핵심.)

### lock 비율도 기존 로그 줄에 추가
절대값(`lockedVertices=1278`, `lockedPositions=848`)만으로는 압력이 안 보이니, **분모 대비 비율을 같은 줄에** 넣어 경계 lock 압력(= Pillar A가 노리는 대상)을 한눈에 보이게 한다.

- `SimplifyGroupGeometry` 줄: `lockedVertices`와 `vertices`(=`SourceVertexCount`)가 이미 있으니 비율만 추가:
  ```cpp
  << ", lockedVertexRatio=" << FormatFloat(static_cast<float>(LockedVertexCount) / static_cast<float>(SourceVertexCount))
  ```
  예: `lockedVertices=1278, vertices=1518 → lockedVertexRatio=0.84`.
- `BuildMergedClusterScratch` 줄: `lockedPositions`(=`LockedPositionCount`)와 `positions`(=`PositionNodes.size()`)가 이미 있으니 비율만 추가:
  ```cpp
  << ", lockedPositionRatio=" << FormatFloat(static_cast<float>(OutScratch.LockedPositionCount) / static_cast<float>(OutScratch.PositionNodes.size()))
  ```
  예: `lockedPositions=848, positions=940 → lockedPositionRatio=0.90`.
- 이 두 비율이 1.0에 가까울수록(예: column_c Level 4의 0.84/0.90) 경계 lock으로 reduce가 막힌 것 → A 적용 후 이 값이 크게 떨어지는지로 효과를 바로 검증.

## 6. 트레이드오프 & 튜닝 레버

- **UV 늘어짐(smearing)**: Permissive가 UV를 가로질러 collapse하면 LOD에서 텍스처가 늘어질 수 있음. 하드 Protect 대신 **UV attribute weight 상향**으로 대응:
  - `SimplifyGroupGeometryWithAttributes`의 `AttributeWeights = {0.5,0.5,0.5(normal), 1.0,1.0(uv)}`에서 UV 1.0 → 2.0~4.0.
  - weight를 올리면 reduce가 약해질 수 있으니(parents 증가) 로그로 트레이드오프 확인.
- **실루엣 블록감**: 이 reduction 레벨에선 상당 부분 불가피. flat-shaded라 더 과장됨 → **smooth normal**이면 같은 폴리곤도 훨씬 둥글게 보임.
- **Force Mip 2를 코앞에서 보는 건 worst-case 디버그 뷰**: `relativeError ≈ 0.01`이라 Level 2는 원래 기둥이 멀(작)을 때만 선택됨. 실사용 거리 평가는 **Force Mip OFF**로.

## 7. 근본 해결 옵션(선택)

column_c가 *의도적으로* faceted가 아니라면(형제 기둥은 smooth) **콘텐츠/임포트 문제**:
- 소스에서 smooth shading으로 **재익스포트** (가장 깨끗, 외형까지 개선), 또는
- import에 **angle-weighted smooth normal 재계산** 추가. 단 기존 `FMesh::GenerateNormalsIfMissing`은 **정점 인덱스 단위 누적**이라 pre-split 메시엔 무효 — 반드시 **position으로 weld한 뒤** 각 가중 누적 + 각 임계로 hard/soft 재판정해야 함.

## 8. 참고: meshopt가 진짜 wedge 모델인가?
그렇다. meshopt 1.1은 내부적으로 position-weld + per-position wedge 루프를 쓴다. 따라서 **커스텀 wedge-aware reducer는 불필요** — `SimplifyPermissive`가 그 동작(seam을 제약이 아니라 비용으로, wedge가 collapse를 따라감)을 제공한다. 커스텀 reducer는 *정말 hard-edge로 유지해야 하는* 에셋을 더 깊은 LOD로 줄이려는 별도 목표가 생겼을 때만 고려.

## 9. 검증 상태 (이전 구현 기준)
- **빌드 OK**: Debug x64 컴파일/링크 통과.
- **DAG OK**: column_c가 4-레벨(98→46→25→12→root) 생성, `ValidateMonotonicErrors` 통과(`coverageFailures=0, boundsMismatches=0`).
- **시각 검증 미완(다음 agent가 할 것)**:
  1. column_c UV 줄무늬(생기면 §6 UV weight).
  2. multi-child root LOD 전환 crack/hole(멀리서 orbit).
  3. 다른 primitive 회귀 없음(Permissive는 전 메시 적용 — reduce를 *더* 허용하는 방향이라 보통 안전).

## 10. 주요 파일 / 심볼
- `Source/Scene/ClusterDAGBuilder.cpp`
  - `SimplifyGroupGeometryWithAttributes` — meshopt 호출, options, attribute weight, 성공 게이트, 로그.
  - `SimplifyMergedClusterGroup` — group 단위 진입, pass-through, `Scratch` 보유.
  - `BuildClusterDAGForPrimitive`의 레벨 루프 — no-progress 가드(§4-2), root 닫기, `GClusterDAGBuildSemanticVersion`.
  - `BuildMergedClusterScratch`/`BuildMergedClusterGeometry`(`MergedClusterSimplifier.cpp`) — position-weld scratch, full-attribute split 출력.
- `Source/Scene/ClusterDAG.h` — `FClusterDAG::IsValid()`, `GClusterDAGMaxRootChildRefs`.
- `Shaders/ClusterDag/ClusterDagCommon.hlsl` — `kClusterDagLevelSplitMaxChildRefsPerGroup = 256`.
- `Shaders/ClusterDag/InitClusterDag*Queues.hlsl` — root를 그룹으로 seed(변경 불필요).
- `ThirdParty/meshoptimizer/simplifier.cpp` — `classifyVertices`, `VertexKind`, `kCanCollapse`, Permissive 블록.
- `ThirdParty/meshoptimizer/meshoptimizer.h` — `meshopt_SimplifyPermissive`, `meshopt_SimplifyVertex_Protect`.

## 11. 강제-수렴(force-fit) 전환 계획

### 목적
현재 group simplify가 목표 parent 수를 못 맞추면(`insufficient_parent_reduction` / `predicted_parent_count_exceeded`) 해당 primitive의 ClusterDAG 빌드를 skip한다. 실패의 두 원인을 없애 항상 수렴하게 만든다.
(overshoot band와 pass-through는 이미 제거된 상태이므로 이 계획 범위 밖. 남은 작업은 A·B 둘.)

### A. 진짜 열린 경계는 lock하지 않는다 (핵심)
`BuildMergedClusterScratch`는 merged-set 안에서 `IncidentTriangleCount == 1`인 에지를 external로 보고 양 끝 position node를 lock한다. 이건 두 가지를 구분하지 못한다:
- **inter-cluster 공유 경계** (이 그룹 밖 클러스터의 삼각형과 맞닿음) — lock 필요(인접 LOD crack 방지).
- **진짜 열린 경계** (어떤 삼각형과도 안 닿는 메시 silhouette) — lock 불필요.

수정: 전역 per-edge 정보를 함께 본다.

| merged-set count | 전역 incident | lock? |
|---|---|---|
| 2 | — | free (그룹 내부) |
| 1 | 전역 ≥ 2 (다른 클러스터와 공유) | **lock** |
| 1 | 전역 == 1 (짝 없음 = 진짜 경계) | **free** |
| — | 전역 > 2 (non-manifold) | lock |

- 데이터 소스: `BuildClusterExternalEdgeOwners(Dag, ClusterIndices)`가 이미 level 전체의 position-keyed 에지 owner 맵을 만든다. 이걸 `BuildMergedClusterScratch`에 넘기거나 같은 키(`FPositionEdgeKey`)로 조회.
- ✅ 확인됨: `BuildClusterExternalEdgeOwners`의 `DistinctOwnerCount`는 **distinct 클러스터 수**다(클러스터마다 edge를 set으로 dedup한 뒤 +1). 따라서 `==1`=한 클러스터만 보유, `>=2`=둘 이상 공유.
- ⚠️ 이 분류는 **`IncidentTriangleCount==1`(merged-set) 전제와 AND일 때만 유효**: 한 클러스터 내부 manifold edge도 ownerCount==1이지만, 그건 merged-set에서 `IncidentTriangleCount==2`라 external 후보에서 이미 빠지므로 안전. 반드시 두 조건을 AND로.
- **watertight 보존**: inter-cluster 에지는 계속 lock → 그 endpoint vertex도 묶인다. silhouette이 클러스터 경계에서 만나는 junction vertex는 inter-cluster 에지의 endpoint이므로 자동 lock → 클러스터 간 crack 없음. 풀리는 건 *한 클러스터 내부에만 있는* 진짜 경계 에지뿐(silhouette만 단순해지고 구멍은 안 생김).
- 기대 효과: column_c Level 4의 848 external(전부 진짜 경계)이 대부분 free → 90% lock 해소 → 12에서 더 줄어듦. A 단독으로도 수렴이 크게 개선될 가능성.

### B. 수렴 바닥에서 실패 대신 수용
budget(`DesiredParentCount` ≈ `ceil(tris/(2·MaxClusterTriangles))`)을 **목표로 유지**한다. retry 루프는 이미 그 목표를 향해 점점 더 공격적으로 simplify한다(`TargetClusterTriangles`를 `Max-2`부터 `MaxClusterTriangles/2` floor까지 −2씩 낮춰 meshopt에 더 적은 target 삼각형을 요청). 즉 **budget을 버리는 게 아니라, 못 맞추면 더 깎아서 맞추는 구조가 이미 있다.**

**바꾸는 건 종료 동작 하나뿐:**
- 현재: floor까지 가도 budget을 못 맞추면 → **실패(build skip).**
- 변경: floor에서 → 실패 대신 **(가장 공격적으로 깎인) 최선 결과를 수용.**

- `predicted < child`는 *"아무거나 수용"의 허가증이 아니라* **DAG가 줄긴 줄어야 한다는 최소 안전 조건**이다. 실제 채택값은 floor의 강제 simplify 결과라 거의 항상 budget 근처 → 약한 감소(예: 30→28)는 나오지 않는다.
- A가 진짜 경계를 풀면 floor 전에 대부분 budget에 도달 → 이 floor-수용 경로는 드물게만 쓰임.

### 안전망
극단적으로 `predicted < child`조차 불가능한 메시 대비, multi-child root(§4-3.1)는 최종 안전망으로 유지.

### 횡단 관심사
- **결정론**: 전역 edge 맵 순회/lock 판정이 해시 순서에 의존하지 않게(키 정렬 또는 결정적 조회). lock 집합이 바뀌므로 `GClusterDAGBuildSemanticVersion` bump 필요.
- **`ValidateMonotonicErrors`**: 진짜 경계 collapse로 silhouette 에러가 커질 수 있음 → parent LODError 반영·단조성 유지 확인.
- **silhouette popping**: 원거리 LOD에서만 영향. Force Mip OFF 실거리에서 평가.
- **crack**: A의 watertight 논리(junction vertex 자동 lock)가 실제로 성립하는지 LOD 전환 orbit로 검증.

### 시퀀스 (각 단계 측정)
1. **A만 먼저** 구현 + 로그에 `openBoundaryEdges`/`lockedBoundaryEdges` 분리 출력. **A의 효과는 lock 비율(lockedPositionRatio/lockedVertexRatio, lockedBoundaryEdges) 하락으로 측정한다.** (A 후에도 build skip이 남을 수 있음 — 그건 A 실패가 아니라 strict budget 게이트 문제이고 B에서 해소. lock 비율이 떨어졌으면 A는 성공한 것.)
2. **그다음 B**: 종료를 "floor에서 실패→수용"으로 전환. 단일/소수 root 수렴 + build skip 사라짐 확인.
3. 각 단계 후 Debug x64 빌드 + sponza 전체 회귀(타 primitive 깨짐 없나) + column_c 시각.

### 테스트
- 빌드 `error C`/`LNK` 없음, version bump 후 `.vmesh` 재생성.
- column_c가 `build skipped` 없이 단일(또는 소수) root로 완료, `coverageFailures=0 / boundsMismatches=0`.
- 2회 빌드 `.vmesh` 바이트 동일(결정론).
- column_c UV·silhouette, LOD 전환 crack/hole, 타 primitive 회귀.

### 리스크 / 열린 질문
- **non-manifold 갭(기존)**: edge가 그룹 내 클러스터에 삼각형 2개 + 그룹 밖에 1개면 merged-set count==2라 external이 안 됨 → 그룹 밖과 공유인데도 lock 안 됨. A가 만든 게 아니라 기존 로직의 갭이고 non-manifold(`nonManifoldEdges>0`)에서만 발생 → 별도 추적(blocker 아님).
- `bExternal`(진단/카운트)와 `bLocked`(meshopt lock) 디커플링 확인 — `bExternal=true`/`ExternalEdgeCount++`는 유지하고 lock만 조건부. `PositionNode.bLocked`가 `BuildMergedClusterGeometry`의 `OutVertexLocks`로만 흘러가는지(다른 watertightness 용도로 안 쓰이는지) 확인.
- A가 inter-cluster를 과소/과대 lock하면 crack 또는 수렴 불가 → lock 카운트 로그로 모니터.
- floor 수용 시 `predicted < child`조차 불가능한 극단 메시 → multi-child root 안전망으로 흡수.

## 12. 장기 아키텍처 옵션: whole-mesh (cross-primitive) ClusterDAG

### 배경 — 왜 필요한가
현재 빌더는 **per-primitive**로 DAG를 만든다 (`BuildClusterDAGForPrimitive`가 primitive마다 따로 호출, 각자 root). 그래서 column_c(primitive 9, 7088 tri)처럼 **작고 degenerate한 primitive를 고립시켜 단독으로 root까지** 줄여야 한다. 작은 고립 메시는 boundary 비중이 높고(boundaryEdgeRatio→1) 수렴이 어렵다.

**대안: 메시의 모든 primitive를 하나로 합쳐 단일 DAG로 빌드한다.** 그러면 degenerate한 작은 조각이 공간적으로 가까운 이웃 지오메트리와 같은 클러스터/그룹에 묶여 **희석**되고, 전체가 함께 줄어 단일(또는 소수) root로 수렴한다. (검증: 같은 에셋을 단일 StaticMesh로 빌드하는 참조 구현은 클러스터가 material 경계를 가로지른다 — `1 Material` 외에 `2/3/4+ Materials` 클러스터가 다수 존재. 즉 클러스터링이 per-section이 아니라 전체 지오메트리 기반이라는 증거.)

### 필요한 작업 (5개)

1. **지오메트리 병합 (빌더)**: 모든 primitive의 position/normal/uv/tangent/index를 하나의 버퍼로 concat + **삼각형마다 material ID** 부여. meshlet 빌드(`meshopt_buildMeshlets`)와 grouping(`meshopt_partitionClusters`)은 이미 공간 기반이라 합친 버퍼에 그대로 동작 → 클러스터가 primitive·material을 가로지름.

2. **🔴 클러스터당 multi-material (runtime resolve) — 가장 큰 난관**: 현재 `ClusterDagResolve.hlsl`은 **클러스터당 material 1개**를 가정한다 (`drawData.ModelIndex → sceneData → material`). whole-mesh면 한 클러스터에 material이 여러 개라 **삼각형(primitiveID)별 material 조회**가 필요하다.
   - 권장: **fast/slow 두 경로**. fast = 클러스터 내 material 1개(=현재 경로 그대로), slow = 클러스터에 material 테이블 + per-triangle material index를 인코딩해 조회. resolve가 분기.
   - **데이터 포맷 + 셰이더 양쪽 변경**이라 이 옵션의 핵심 비용. 런타임이 multi-material 클러스터를 못 그리면 화면이 깨진다.

3. **simplify의 material 경계 처리**: 그룹 simplify에서 **material 경계를 넘는 collapse 방지**(material 경계를 lock 또는 attribute seam으로). A의 boundary-lock 메커니즘에 material 경계 lock을 추가하는 식. 안 그러면 material이 번진다.

4. **수렴 메커니즘**: 합치면 degenerate 조각이 이웃 클러스터와 같은 그룹에 들어가 희석되고, 마지막엔 multi-component blob을 ≤cluster_size로 줄여 단일 root로 수렴(클러스터는 연결성 불필요 — triangle 수 제한만 충족하면 됨). disconnected component가 한 클러스터에 섞여도 무방.

5. **런타임/스트리밍**: DAG가 primitive당 N개 → 메시당 1개. traversal/page/culling은 단일 DAG라 일부 단순해지지만, DrawData/material 바인딩 구조를 손봐야 함.

### 규모 / 권장
- A/B/multi-child root와 **차원이 다른 대형 작업**. 특히 **#2(multi-material resolve, fast/slow path)**가 데이터 포맷+셰이더를 관통하는 큰 변경.
- **단기 출시 경로는 multi-child root** (column_c를 buildable하게 닫음, 얕은 DAG지만 동작/표시 정상).
- **whole-mesh는 별도 대형 과제(로드맵 항목)**: per-primitive 고립이 degenerate primitive 수렴 난점의 근본 원인이므로, LOD 품질을 근본적으로 올리려면 이 방향. 착수 시 **#2부터** 설계(나머지는 #2가 정해지면 따라옴).

## 13. whole-mesh 구현 계획 (착수용)

### 현재 상태
- A(경계 lock owner 분류) ✅, B(force-fit floor 수용) ✅ 완료.
- multi-child root 안전망은 **건너뜀**(이 경로 대신 whole-mesh로 직행).
- 구조: per-primitive. `FMesh::ClusterDAGs = std::vector<FClusterDAG>`(primitive당 1개), `BuildClusterDAGForPrimitive`가 primitive마다 호출([ClusterDAGBuilder.cpp:1762](Source/Scene/ClusterDAGBuilder.cpp#L1762), 루프 [:3394](Source/Scene/ClusterDAGBuilder.cpp#L3394)).

### 🔑 핵심 발견 — 멀티-머티리얼 클러스터 인프라가 이미 부분적으로 존재 (→ #2 난이도 축소)
`FRuntimeCluster`에 **`DrawDataStart` + `DrawDataCount`**가 있고, 직렬화/페이지가 클러스터당 여러 DrawData를 루프함([:2807](Source/Scene/ClusterDAGBuilder.cpp#L2807)). 그런데 빌더가 **[:734](Source/Scene/ClusterDAGBuilder.cpp#L734)에서 `DrawDataCount = 1`로 하드코딩** — *포맷은 멀티 DrawData 지원, 현재는 클러스터당 1개만 사용*.

→ **multi-material 클러스터 = `DrawDataCount > 1`(머티리얼별 삼각형 range 1개씩)**로 표현 가능. resolve는 이미 `visibleEntry.DrawDataIndex → ModelIndex → material`로 DrawData별 머티리얼을 본다. 따라서 **새 per-triangle 셰이더 경로를 만들 필요 없이 기존 draw/resolve 메커니즘 재사용** → §12 #2가 "셰이더 대수술"에서 "빌더가 머티리얼별 DrawData range 분할 + 런타임이 클러스터당 DrawData 루프"로 축소.
- ⚠️ **Phase 0 선검증**: visibility 패스가 클러스터의 **각 DrawData range마다 draw/visibleEntry를 생성**하는지 확인. 성립해야 multi-material이 그대로 동작.

### Phase 계획
- **Phase 0 — 검증**: visibility가 DrawData range별 draw를 만드는지 확인. (#2 전제)
- **Phase 1 — 빌더 병합**: `BuildClusterDAGForMesh` 신설 — 모든 primitive의 position/normal/uv/tangent/index를 1버퍼로 concat + **per-triangle material ID** 배열. `CompactAndOptimizeBuilderGeometry`로 position-weld(인접 primitive 연결). material은 vertex 키에 넣지 말고 per-triangle. per-primitive 경로는 플래그로 유지(비교/fallback).
- **Phase 2 — material 관통**: meshlet 빌드/grouping은 공간 기반이라 그대로(클러스터가 material 가로지름). simplify는 **material 경계를 lock**(A의 boundary-lock owner 분류에 material 경계 추가) → 경계 안 넘으니 각 출력 삼각형 material 명확. 출력 삼각형→material 대응을 source region으로 복원.
- **Phase 3 — 클러스터당 멀티 DrawData (#2 본체)**: 클러스터 삼각형을 material로 정렬 → 연속 run마다 DrawData 1개 생성, `DrawDataCount` 실제값으로(1 하드코딩 제거). 직렬화/페이지는 거의 그대로.
- **Phase 4 — 런타임**: DAG가 mesh당 1개(traversal/page 단순화). DrawData/material 바인딩이 클러스터당 N개 DrawData를 그리는지 확인(Phase 0과 동일).
- **Phase 5 — 검증/정리**: column_c가 씬에 흡수돼 단일(또는 소수) root 수렴, material 안 번짐, 타 메시 회귀 없음. `GClusterDAGBuildSemanticVersion` bump.

### 권장 착수 순서 (derisk)
1. **Phase 0 검증** (visibility의 DrawData-range별 draw 여부).
2. **Phase 3를 먼저 작게**: 기존 per-primitive 빌더에서 인위로 한 클러스터에 DrawData 2개를 넣어 멀티-머티리얼 클러스터를 렌더 → 가장 위험한 런타임 부분 선제 검증.
3. 되면 Phase 1(병합) → 2(material 관통/simplify lock) → 4 → 5.

### 리스크
- Phase 0이 거짓이면(클러스터당 단일 draw 가정) #2가 §12대로 커짐(per-triangle 셰이더 경로 필요).
- simplify 통과 후 출력 삼각형→material 대응 복원 방식(메커니즘 미확정) — Phase 2의 핵심 설계.
- 병합 시 정점/인덱스 규모 증가, 결정론(병합 순서 고정), .vmesh 포맷/버전.
