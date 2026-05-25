# ClusterDAG Builder 개요

## 목적

이 문서는 UncleRenderer에 현재 구현된 ClusterDAG builder 작업을 정리하고, builder 쪽에서 계속 챙겨야 할 작업을 기록합니다.

runtime traversal, visibility-buffer 렌더링, streaming은 의도적으로 자세히 다루지 않습니다. 해당 영역은 별도 코드와 문서에서 관리합니다. 이 문서는 아래 범위에 집중합니다.

- `Source/Scene/ClusterDAG.h`
- `Source/Scene/ClusterDAGBuilder.cpp`
- `Source/Scene/MergedClusterSimplifier.h`
- `Source/Scene/MergedClusterSimplifier.cpp`
- builder correctness에 영향을 주는 `.vmesh` builder/cache 출력

## 현재 Builder 범위

현재 ClusterDAG builder는 렌더러의 현재 runtime 요구사항에 맞춘 cluster hierarchy 설계를 따릅니다.

구현된 목표는 다음과 같습니다.

- static mesh primitive에서 meshlet leaf cluster 생성
- cluster를 더 거친 parent level로 group화하고 단순화
- 하나의 root cluster와 하나의 root group으로 수렴
- CPU hierarchy data와 compact runtime hierarchy data 인코딩
- 이후 runtime fetch와 visibility resolve에 필요한 packed geometry data 보존
- 결과를 `.vmesh`에 캐싱

현재 한계는 다음과 같습니다.

- simplification은 주로 MeshOpt 기반
- position-only simplification fallback은 사용하지 않음
- skinned / alpha-blended runtime policy는 builder 밖에서 처리
- production 수준의 mesh simplification, page layout, validation rule은 아직 발전 중

## 주요 Build 흐름

`BuildClusterDAGForPrimitive`는 primitive 하나에 대한 main entry point입니다.

현재 흐름은 크게 세 단계로 나뉩니다.

초기 leaf level 생성:

1. primitive 입력 검증
2. source vertex stream과 index 복사
3. `CompactAndOptimizeBuilderGeometry` 실행
4. compacted base stream을 `FClusterDAG`에 추가
5. `BuildMeshletClustersForGeometry`로 leaf cluster 생성
6. leaf cluster마다 leaf `FClusterGroup` 생성

반복 reduction loop:

1. cluster가 하나보다 많은 동안 level을 하나씩 올리며 반복
2. current cluster를 group으로 partition
3. group마다 merged scratch topology 생성
4. 각 group 단순화
5. 단순화된 group geometry에서 parent meshlet cluster 재생성
6. group과 parent cluster 연결
	- group의 입력 child cluster 집합은 `ChildClusters`
	- group의 출력 parent cluster 집합은 `ParentClusters`
7. 이번 level의 parent cluster들을 다음 iteration의 current cluster로 사용

마무리 단계:

1. convergence, monotonic error, group sphere coverage 검증
2. root group 생성
3. `FRuntimeClusterHierarchy` 인코딩
4. cache/runtime용 packed vertex data 생성

핵심 개념 분리는 다음과 같습니다.

- `FCluster`는 실제 renderable cluster입니다.
- `FClusterGroup`은 child와 생성된 parent cluster를 연결하는 hierarchy node입니다.

## LeafGroup / ClusterGroup 과 ChildRef / ParentRef

이 문서에서 헷갈리기 쉬운 지점은 `FClusterGroup`의 `ChildRefs`/`ParentRefs`가 일반적인 트리 노드의 "부모-자식 포인터"와 완전히 같은 의미가 아니라는 점입니다.

현재 builder에서의 의미는 다음과 같습니다.

- `ChildRefs`: 이 group이 대표하는 더 세밀한 cluster 집합
- `ParentRefs`: 이 group을 단순화해서 생성된 다음 level cluster 집합

즉, `FClusterGroup`은 "현재 level에서 묶인 child cluster 집합"과 "그 결과로 생성된 parent cluster 집합"을 동시에 들고 있는 연결 노드입니다.

구체적으로는 다음과 같이 채워집니다.

1. 초기 leaf 단계에서는 cluster 하나마다 leaf group 하나를 만들고, `LeafGroup.ChildRefs`에 자기 cluster 하나를 넣습니다.
2. reduction loop에서는 `Group.ChildRefs = MakeClusterRefs(ChildClusters)`로 입력 cluster 집합을 기록합니다. 여기서 `ChildClusters`는 partition 결과로 만들어진 cluster 묶음(또는 cluster 수가 작아 partition을 건너뛴 경우 current cluster 전체 묶음)입니다.
3. 단순화 + 재메시렛 후 생성된 `ParentClusters`를 `Group.ParentRefs`로 저장합니다.

따라서 초기 leaf group의 `ChildRefs`는 "자기 자신의 단일 구성 cluster"에 가깝고, 일반적인 의미의 하위 트리 자식을 가리키는 포인터라고 보기 어렵습니다.

반대로 reduction loop에서 생성되는 group의 `ChildRefs`는 이전 level cluster(처음에는 leaf cluster)의 묶음이고, `ParentRefs`는 그 묶음을 단순화해 얻은 상위 level cluster 묶음입니다.

runtime 인코딩 시에는 이 둘이 하나의 `Runtime.ChildRefs` 배열에 연속 저장되며, group마다 `ChildRefStart/ChildRefCount`와 `ParentRefStart/ParentRefCount`로 범위를 나눠 참조합니다. 이름은 `ChildRefs` 배열이지만, 실제 payload는 child ref와 parent ref를 모두 포함합니다.

## Leaf Cluster 생성

Leaf cluster는 MeshOpt meshlet generation으로 생성합니다.

- max vertices 기본값은 128
- max triangles 기본값은 128
- meshlet output은 `FCluster` record가 됨
- cluster-local triangle index는 byte로 compact 저장
- cluster vertex는 공유 DAG vertex stream을 참조

## Grouping과 Partitioning

각 reduction level에서 다음을 수행합니다.

- 현재 cluster 수가 충분히 작으면 grouping을 skip하고 모든 current cluster를 하나의 group으로 묶음
- 그렇지 않으면 `meshopt_partitionClusters`로 current cluster를 partition
- 너무 작은 partition은 adjacency를 사용해 병합

현재 adjacency 구현은 lightweight edge-owner map을 사용합니다. `ComputeClusterAdjacency`는 edge의 첫 owner를 기록하고, 이후 cluster가 같은 edge를 공유하면 adjacency를 증가시킵니다.

알려진 gap은 다음과 같습니다.

- 셋 이상의 cluster가 공유하는 non-manifold edge는 참여한 모든 cluster 사이를 더 완전하게 link해야 함
- scratch topology는 non-manifold edge를 count하지만, partition adjacency에는 아직 더 완전한 non-manifold 처리가 필요함

## Merged Scratch Topology 구성

`BuildMergedClusterScratch`는 group simplification을 안정적으로 수행하기 위한 position 기반 topology를 만듭니다.

Scratch data는 다음으로 구성됩니다.

- `FScratchCorner`: source corner provenance와 position node index
- `FScratchTriangle`: corner index와 position-node triangle index
- `FScratchPositionNode`: unique position과 lock flag
- `FScratchEdge`: position-node edge, incident triangle count, external flag
- `FMergedClusterScratch`: 모든 scratch array와 active triangle, external edge, locked position, non-manifold count

이 구조가 중요한 이유는 일부 glTF asset이 사실상 per-corner vertex mesh이기 때문입니다. 이런 asset에서는 emitted vertex/index stream이 거의 shared topology를 갖지 않아 direct MeshOpt simplification이 진행되지 않을 수 있습니다.

Scratch topology는 exact position bit 기준으로 dedup하여 MeshOpt가 단순화할 수 있는 position-welded topology를 제공합니다.

## Simplification 경로

Group simplifier는 두 path를 시도합니다.

### Attribute Stream MeshOpt

먼저 builder는 일반 merged stream을 emit하고 attribute를 포함한 MeshOpt simplification을 호출합니다.

이 path는 source attribute를 더 직접적으로 보존하므로 우선 사용합니다. 다만 stream이 지나치게 disconnected되어 있으면 실패하거나 zero progress가 발생할 수 있습니다.

### Position-Only Fallback Policy

Attribute stream path가 실패하면 builder는 현재 group reduction을 실패로 처리합니다. 이전의 scratch `PositionNodes` 기반 position-only fallback은 UV seam, normal/tangent seam, material boundary에서 attribute wedge를 잃을 수 있어 사용하지 않습니다.

정공법은 UE NaniteBuilder와 같은 방향입니다.

- position-welded connectivity는 유지해 disconnected per-corner mesh에서도 reduction이 진행되게 함
- UV/normal/tangent/material wedge는 별도 provenance로 추적해 seam을 보존함
- target triangle count를 낮춰가며 재시도하고, 최종적으로 `MaxClusterTriangles` 이하 parent cluster로 수렴시킴

즉 다음 단계의 reducer는 "position node + wedge/corner attribute"를 동시에 다루는 단일 경로여야 합니다. 단순한 first-valid-corner provenance는 더 이상 fallback 정책으로 사용하지 않습니다.

## Parent Budget과 수렴 조건

Builder는 다음 값을 계산합니다.

- source triangle count에서 desired parent count 계산
- desired parent count와 child cluster count에서 max allowed parent count 계산

Max allowed count는 실제 convergence를 요구하면서도 어느 정도 overshoot을 허용합니다. 이 값은 `ChildClusterCount - 1`로 cap되므로, valid result는 반드시 cluster count를 줄여야 합니다.

이는 exact desired parent count가 너무 엄격한 어려운 group에서 중요합니다. simplifier가 desired count에 딱 맞추지 못하더라도 child보다 적은 parent cluster를 만들 수 있으면 계속 수렴할 수 있습니다.

Group reduction은 다음 조건을 모두 만족할 때만 accept됩니다.

- triangle count가 감소함
- predicted parent count가 0이 아님
- predicted parent count가 child cluster count보다 작음
- 재생성된 parent cluster가 비어 있지 않음
- actual parent count가 child cluster count보다 작음
- actual parent count가 max allowed budget 안에 있음

## Boundary Lock 처리

External scratch edge는 endpoint position node를 lock합니다.

Builder는 다음 조건에서 더 부드러운 boundary behavior를 허용할 수 있습니다.

- boundary lock pressure가 높음
- 현재 level의 모든 current cluster가 하나의 group에 들어 있음
- normal scratch attempt가 zero progress 또는 parent-budget excess 때문에 실패함

이 처리는 late level에서 locked-position ratio가 매우 높아 모든 boundary lock을 보존한 채 충분히 collapse하지 못하는 mesh를 위해 추가되었습니다.

## Bounds와 Error

Builder는 서로 관련된 여러 값을 추적합니다.

- cluster bounds: 개별 `FCluster`가 실제로 포함하는 기하 범위를 나타내는 구(sphere)입니다. group coverage와 runtime cluster culling의 기준이 됩니다.
- group bounds: 하나의 `FClusterGroup`이 가진 `ChildRefs` 전체를 감싸는 기하 범위 구입니다. "이 group이 어떤 child cluster들을 공간적으로 대표하는지"를 표현합니다.
- LOD bounds: LOD 선택/전파용 경계 구입니다. group/cluster의 단순한 기하 bounds와 분리해 관리하며, parent 생성 시 상위 LOD 판단 기준으로 전달됩니다.
- simplify error: 현재 group을 단순화하는 단계에서 새로 발생한 오차입니다. 이번 reduction 단계 자체가 추가한 품질 손실량을 의미합니다.
- inherited child error: child cluster가 이전 단계들에서 이미 물려받아 누적된 최대 오차입니다. 현재 단계는 이 값을 child의 자체 LOD error와 함께 고려합니다.
- parent group error: 상위로 전달할 최종 error 값입니다. `max(child 쪽 누적 error, simplify error)`로 계산해 error monotonicity를 유지합니다.

Build 후에는 다음을 검증합니다.

- group child/parent link를 따라 error가 monotonic인지
- group sphere coverage가 유효한지

이 검사는 중요합니다. 잘못된 bounds 또는 non-monotonic error는 runtime LOD selection에서 cluster가 누락되거나 너무 늦게 refine되는 문제로 이어질 수 있습니다.

## Runtime Hierarchy 인코딩

Runtime traversal 자체는 이 문서 범위 밖이지만, builder는 runtime hierarchy를 생성합니다.

`EncodeRuntimeClusterHierarchy`는 다음을 기록합니다.

- runtime groups
- runtime clusters
- runtime child refs
- runtime draw data
- packed indices
- root group index

Builder correctness에는 이 데이터가 self-consistent하도록 만드는 것까지 포함됩니다. invalid child ref, draw-data range, packed index range가 `.vmesh`에 저장되면 runtime bug를 진단하기가 훨씬 어려워집니다.

## Packed Vertex Data

`BuildClusterDAGPackedVertexData`는 compact vertex stream을 생성합니다.

- quantized position words
- octahedral normal encoding
- packed UVs
- packed tangents
- packed colors
- constant UV/color fallback data

현재 cache path는 runtime fetch를 위해 full original DAG vertex stream 대신 packed data를 저장합니다. 따라서 packed-data validation도 builder correctness의 일부입니다.

## Cache 출력

`.vmesh` cache는 현재 runtime과 이후 pass에 필요한 builder output을 저장합니다.

- clusters
- groups
- runtime hierarchy arrays
- packed vertex data
- cluster-local triangle indices
- cluster vertex references
- page directory와 page payload data

현재 version constant는 `ClusterDAGBuilder.cpp`에 있습니다.

- `.vmesh` version: `16`
- ClusterDAG build semantic version: `5`

## 해야 될 일

- `MergeSmallPartitions`는 당장은 유지한다. 현재 builder에서는 `meshopt_partitionClusters`가 최소 그룹 크기나 locality link를 보장하지 않고, reduction gate도 `child cluster count`보다 적은 parent만 허용하므로 작은 잔여 그룹을 사후 보정하는 단계가 아직 필수다.
- `MergeSmallPartitions`의 역할을 코드 주석과 문서에 명시한다. "meshopt partition 이후 생기는 singleton / undersized group을 strict reduction 전에 정리하는 보정 단계"라는 점을 분명히 남긴다.
- position-only fallback을 되살리지 않는다. 이 경로는 position-welded topology에서 attribute wedge를 잃어 UV seam 손상을 만들 수 있으므로 품질 파괴 fallback으로 취급한다.
- UE식 단일 reducer를 설계한다. position-welded connectivity와 per-corner/wedge attribute provenance를 동시에 유지하고, target triangle count를 낮춰가며 재시도해 단일 root 수렴을 보장하는 방향이다.
- attribute simplification 실패가 잦은 asset에 대해 diagnostic을 강화한다. 실패 group, locked vertex count, zero-progress attempt, predicted parent count, seam 후보 수를 추적한다.
- locality-aware grouping 입력을 검토한다. 비연결 섬을 partition 전에 공간적으로 연결해서 undersized group 발생 자체를 줄이는 방향이다.
- wedge-aware reducer와 locality-aware grouping이 들어간 뒤에는 `MergeSmallPartitions`를 "정합성 필수 단계"에서 "품질 보정 단계"로 강등하고, 최종적으로 제거 가능한지 다시 평가한다.

## 향후 최적화 작업 후보

아래 항목은 correctness를 바꾸지 않으면서 빌드 시간과 메모리 할당 압력을 줄일 가능성이 큰 후보들입니다.

### 1) `ExtractAbsoluteClusterIndices` 반복 할당 완화

현재 `ExtractAbsoluteClusterIndices`는 호출마다 새로운 `std::vector<uint32_t>`를 생성해 반환합니다. 이 함수는 인접도 계산, partition 입력 생성, flatten, runtime draw data 재생성, leaf 진단 등에서 반복 호출되므로 대형 메시에서 힙 할당 누적 비용이 커질 수 있습니다.

개선 방향:

- 반환형 대신 output buffer를 인자로 받는 overload 추가
- 호출자 단에서 thread-local 또는 스코프 재사용 버퍼 도입
- 필요 시 cluster index expansion 캐시(빌드 단계 한정) 검토

주의점:

- 캐시 도입 시 invalidation 규칙(클러스터 데이터 변경 지점) 명확화 필요
- 메모리 사용량 증가와 할당 감소 사이의 trade-off 측정 필요

### 2) Primitive 단위 빌드 병렬화

`FMesh::BuildClusterDAGs`는 현재 primitive를 순차 처리합니다. primitive 간 DAG 생성은 구조적으로 독립적이므로 병렬화 시 체감 빌드 시간 단축 가능성이 큽니다.

개선 방향:

- primitive loop를 병렬 실행(`ParallelFor` 또는 task group)
- primitive별 임시 상태 완전 분리 후 최종 결과만 index 순서대로 commit
- 로그/통계 집계 경합 최소화

주의점:

- 결과 순서 결정성(determinism) 유지 필요
- 공용 allocator/로그 출력 경합으로 인한 역효과 여부 측정 필요

### 3) `MergeSmallPartitions` 후보 탐색 비용 절감

`MergeSmallPartitions`의 후보 탐색 루프에서 group center 계산이 후보마다 반복되고, shared-edge 집계가 그룹/후보 조합마다 다중 조회를 수행합니다. 작은 partition 정리 단계라 평균 비용은 제한적이지만, group 수가 많을 때는 누적 비용이 커질 수 있습니다.

개선 방향:

- group center를 pass 시작 시 캐싱하고 merge 시에만 갱신
- 필요 시 group-pair shared-edge 누적값 캐싱(merge 시 invalidate/update)

주의점:

- merge 후 캐시 갱신 누락 시 오동작 위험
- 구현 복잡도 대비 실측 이득이 충분한지 프로파일로 확인 필요

## 작업 우선순위 메모

초기 착수 우선순위는 다음을 권장합니다.

1. `ExtractAbsoluteClusterIndices` 재사용 버퍼화 (낮은 위험, 적용 범위 넓음)
2. `MergeSmallPartitions` center 캐싱 (로컬 변경, 효과 확인 쉬움)
3. primitive 병렬화 (효과가 가장 클 수 있으나 결정성/경합 검증 필요)

Cache invalidation은 다음을 사용합니다.

- source file write time
- source file size
- build parameter hash
- semantic/cache version

`ForceRebuildClusterDAGCache` / `ForceClusterDAGCacheBuild` / `IgnoreClusterDAGCache` 계열 config alias는 `.vmesh`가 이미 있어도 재생성을 강제합니다.

## Builder 후속 작업

### Correctness 개선

- `ComputeClusterAdjacency`를 다시 점검하고 non-manifold shared edge를 모든 participating cluster 사이에서 일관되게 link하기
- cache 저장 전에 builder-side hierarchy validator 추가
- 모든 group child ref, parent ref, cluster index, runtime child-ref range, draw-data range, packed index range 검증
- cache file을 load하고 `RootGroupIndex`부터 hierarchy를 순회하는 `.vmesh` offline validator 추가
- root group에서 도달할 수 없는 cluster에 대한 명시적인 diagnostic 추가

### Simplification 품질

- position-welded connectivity와 UV/normal/tangent wedge를 동시에 추적하는 reducer 추가
- target triangle count를 낮춰가며 재시도하고 최악에는 `MaxClusterTriangles` 이하 parent cluster로 수렴시키는 UE식 retry loop 추가
- 선택된 wedge provenance가 normal 또는 UV를 급격히 바꾸는 triangle에 대한 seam-quality diagnostic 추가
- asset별 zero-progress attempt, parent-budget overshoot, non-manifold edge count 추적

### Bounds와 Error

- `ValidateGroupSphereCoverage` 실패 로그를 더 자세히 추가
- level별 worst LOD-bound delta debug output 추가
- 누락된 cluster reference를 잡기 위해 leaf/full-resolution `ForceMip` output을 direct meshlet output과 비교
- inherited child error와 simplify error를 parent group error로 합치는 방식 재검토

### Cache와 Metadata

- `.vmesh` 저장/로드 시 reducer/build assumption 로그 출력
- cache가 현재 wedge-aware reducer semantics에서 생성되었는지 알 수 있는 metadata 저장
- hierarchy layout, packed vertex layout, simplifier semantics가 바뀔 때 cache semantic version bump를 엄격하게 관리
- mesh count, DAG count, group count, cluster count, root group, packed vertex count, page count를 출력하는 작은 cache-inspection command 또는 debug UI 추가

## 권장 단기 작업 순서

1. builder/cache hierarchy validation 추가
2. non-manifold cluster adjacency behavior 수정 또는 문서화
3. normal/tangent/UV seam을 보존하는 wedge-aware reducer 추가
4. SciFiHelmet 같은 어려운 asset을 위한 build diagnostic 강화
5. `.vmesh` cache metadata logging과 inspection 추가
6. builder validation이 안정된 뒤 visual/runtime check 재실행
