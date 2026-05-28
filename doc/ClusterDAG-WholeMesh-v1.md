# ClusterDAG Whole-Mesh v1

## Summary

Whole-Mesh ClusterDAG v1은 `FMesh` 안의 primitive/section을 각각 따로 줄이지 않고, mesh 단위로 하나의 ClusterDAG를 만든다. 목표는 작은 section이 단독으로 root까지 수렴하지 못하는 상황을 피하고, 같은 node transform을 공유하는 section들을 하나의 기하 구조 안에서 reclustering/reduction하도록 만드는 것이다.

이 버전은 mixed-section cluster를 허용한다. 다만 section 경계를 넘는 collapse는 허용하지 않는다. 즉, 한 cluster 안에 여러 section의 triangle이 같이 들어갈 수는 있지만, simplifier가 서로 다른 section의 vertex를 하나로 접거나 section seam을 무너뜨리면 builder invariant 위반으로 처리한다.

## Build Model

### Mesh-Level DAG

- `FMesh::BuildClusterDAGs`는 v1에서 mesh당 DAG 1개를 생성한다.
- primitive index별 DAG 대신 `ClusterDAGs.front()`가 해당 mesh의 shared DAG 역할을 한다.
- cache semantic version과 payload header를 갱신해서 이전 per-primitive DAG cache를 재사용하지 않는다.

### Section-Aware Vertex Stream

빌드 스트림에는 `VertexSectionIndices`가 추가되었다. 각 compacted vertex는 어느 source section에서 왔는지 보존한다.

이 section id는 다음 경로에 사용된다.

- vertex compact/weld key에 포함되어 section 간 vertex identity를 분리한다.
- merged group geometry 생성 시 output vertex에도 복사된다.
- simplify 후 output triangle의 세 vertex가 같은 section인지 검증한다.
- runtime draw data를 section별로 다시 분리할 때 source of truth로 사용한다.

### Lock Policy

simplifier lock mask는 두 종류의 lock을 OR로 합친다.

```text
FinalLocks = InterClusterBoundaryLocks | SectionBoundaryLocks
```

`InterClusterBoundaryLocks`는 merged-set 내부에서는 외부 edge처럼 보이지만, 현재 level의 다른 cluster와 실제로 공유되는 경계를 보호한다. 이 lock은 인접 LOD crack을 막기 위한 것이다.

진짜 열린 mesh silhouette은 lock하지 않는다. `BuildMergedClusterScratch`는 merged-set 내부 incident가 1인 edge를 먼저 external 후보로 보지만, `BuildClusterExternalEdgeOwners`가 만든 현재 level owner map을 다시 확인한다. owner count가 2 이상이면 inter-cluster shared boundary로 보고 lock하고, owner count가 1이면 진짜 open boundary로 보고 lock하지 않는다. owner map에서 edge를 찾지 못하는 예외 상황은 보수적으로 lock한다.

`SectionBoundaryLocks`는 section seam을 보호하기 위한 lock이다. Section boundary lock은 두 경우에 설정된다.

- 서로 다른 section triangle이 공유하는 edge의 양 끝 vertex를 lock한다.
- 같은 position에 여러 section vertex가 공존하는 seam position을 lock한다.

이 설계는 section 간 collapse를 막지만, section 간 reclustering 자체는 막지 않는다. 그래서 작은 section이 다른 주변 geometry와 같은 group/cluster packing 안에서 수렴할 수 있고, 동시에 material/section 경계는 보존된다.

로그에서는 이 구분을 다음 값으로 확인할 수 있다.

- `openBoundaryEdges`: 진짜 열린 경계로 분류되어 lock하지 않은 edge 수
- `lockedBoundaryEdges`: 다른 cluster와 공유되어 lock한 edge 수
- `missingBoundaryEdges`: owner map에서 확인하지 못해 보수적으로 lock한 edge 수
- `sectionBoundaryEdges`: section seam 보호를 위해 lock한 edge 수

## Simplification Invariants

Whole-mesh simplify 결과는 다음 조건을 만족해야 한다.

- output triangle의 세 vertex section id가 모두 같아야 한다.
- section seam edge가 collapse로 사라지면 안 된다.
- mixed-section cluster는 가능하지만 mixed-section triangle은 불가능하다.

위 조건이 깨지면 fallback geometry를 조용히 만드는 대신, builder invariant violation으로 로그를 남기고 해당 whole-mesh DAG runtime을 비활성화한다. 이는 잘못된 material resolve나 section boundary 손상을 숨기지 않기 위한 정책이다.

## Runtime Model

### Shared DAG Resource

scene loading은 기존처럼 section별 `FSceneModelResource`를 만든다. 다만 whole-mesh DAG가 활성화된 mesh/node에서는 section 0 model이 shared ClusterDAG hierarchy/buffer owner가 된다.

나머지 section model은 base draw fallback에 필요한 model/material state를 유지하되, ClusterDAG runtime에서는 shared DAG에 의해 커버되는 model로 표시된다.

### Section Draw Data

`FRuntimeClusterDrawData::Reserved0`는 `SectionIndex` 필드로 승격되었다.

빌드 단계에서 cluster의 packed index range를 section별로 분리한다. 따라서 mixed-section cluster는 `DrawDataCount > 1`이 될 수 있다.

runtime은 각 draw data의 `SectionIndex`를 section model index로 매핑한 뒤 shader-visible draw data의 `ModelIndex`에 기록한다. 이 덕분에 한 cluster가 여러 section triangle을 포함해도 material resolve는 section별 scene model을 바라본다.

### Base Draw Suppression

whole-mesh DAG가 활성화된 mesh/node는 기존 per-section base draw path를 건너뛰고 ClusterDAG runtime path만 사용한다.

다음 조건에서는 v1 runtime을 비활성화하고 기존 path를 유지한다.

- alpha blend material이 포함된 mesh
- alpha mask material이 포함된 mesh
- skinned mesh
- build invariant violation이 발생한 mesh

## Diagnostics

빌드/런타임 로그는 mixed-section draw data 비용을 추적한다.

- `mixedSectionClusters`
- `drawDataCount`
- `maxDrawDataPerCluster`
- `avgDrawDataPerCluster`

section이 많은 mesh에서는 mixed-section cluster가 draw data 증가로 이어질 수 있다. 이 값들이 과도하게 커지면 cluster packing 또는 section split policy를 다시 조정해야 한다.

## Expected Behavior

Whole-Mesh v1에서 작은 section이 수렴하는 메커니즘은 section seam collapse가 아니다.

의도한 수렴 경로는 다음과 같다.

1. 모든 section triangle을 mesh 단위로 함께 clustering한다.
2. 작은 section triangle이 주변 geometry와 mixed-section group/cluster로 묶일 수 있다.
3. 진짜 열린 경계는 lock하지 않고, inter-cluster 공유 경계와 section seam만 보호하므로 reduction freedom이 늘어난다.
4. section seam은 lock으로 보존되고, runtime draw data가 section별 material resolve를 유지한다.

따라서 결과적으로 DAG는 mesh 단위로 root까지 수렴할 가능성이 커지고, section 경계와 material assignment는 runtime에서 유지된다.

## Validation Checklist

- mesh당 ClusterDAG가 1개만 생성되는지 확인한다.
- 기존 primitive 단독 DAG가 생성되지 않는지 확인한다.
- whole-mesh DAG가 root까지 수렴하는지 확인한다.
- section violation 로그가 0인지 확인한다.
- mixed-section cluster가 있어도 material 경계가 화면에서 유지되는지 확인한다.
- alpha/skinned mesh가 기존 draw path로 fallback되는지 확인한다.
- draw data 증가량이 합리적인지 확인한다.

## Current Limitations

- v1은 같은 node transform을 공유하는 mesh primitive/section을 대상으로 한다.
- section seam은 hard-lock되므로, seam이 지나치게 많은 mesh는 reduction freedom이 제한될 수 있다.
- multi-child root는 별도 fallback 정책으로 다룰 수 있지만, 이 문서의 핵심 범위는 mesh-level DAG와 section-safe runtime draw 분리다.
- 투명/마스크/skinned 경로는 whole-mesh DAG runtime 대상에서 제외된다.

## Multi-Child Root Closure

builder는 whole-mesh DAG를 반드시 "최종 클러스터 1개"로 강제 수렴시키지 않고, 여러 child cluster를 참조하는 root group으로 종료할 수 있습니다. 공유 child-ref 상한은 `Shaders/ClusterDag/ClusterDagShared.h`의 `kClusterDagMaxChildRefsPerGroup`이며, C++에서는 동일 값을 `GClusterDAGMaxRootChildRefs`로 노출합니다.

close 경로는 level preflight에서 다음 상황을 감지했을 때 사용됩니다.

- 자연스러운 수렴 바닥(convergence floor)
- parent reduction 부족
- root-close 임계값을 넘는 relative simplify error

closure는 해당 level의 parent group을 커밋하기 전에 level 입력에서 바로 수행됩니다. 따라서 오차가 큰 상위 레벨이 DAG에 추가되지 않습니다.

closure가 요청된 시점의 current cluster 수가 shared child-ref 상한을 초과하면, builder는 이유를 로그로 남기고 whole-mesh DAG를 실패 처리합니다. 즉, 유효하지 않은 root group을 조용히 생성하지 않습니다.

## Alpha-Mask Visibility

whole-mesh ClusterDAG runtime은 visibility 단계에서 alpha-mask section을 지원합니다. alpha-blend section은 v1에서 계속 whole-mesh runtime을 비활성화합니다. blend는 draw order와 누적 blending이 필요하지만, ClusterDAG visibility buffer는 최종 visible primitive ID/depth만 저장하기 때문입니다.

masked ClusterDAG draw range는 material pipeline key의 alpha-mask bit로 opaque range와 분리됩니다. HW visibility pass는 masked PSO variant에서만 UV와 vertex color alpha를 로드하고, base-color UV transform을 적용한 뒤 base-color texture alpha를 샘플합니다. alpha-test 기준은 base pass와 같은 `BaseColorAlpha * VertexColorAlpha * TextureAlpha < AlphaCutoff`입니다. opaque visibility PSO는 position-only 경로를 유지하고 `[earlydepthstencil]`도 그대로 사용합니다.

SW raster path도 `AlphaMode == Mask`일 때만 같은 alpha-test를 수행합니다. compute shader에는 derivative가 없으므로 barycentric으로 UV/color alpha를 보간하고, texture alpha는 명시적 `SampleLevel(..., 0)`로 샘플합니다. 현재 SW raster는 하나의 visible-entry 리스트를 단일 indirect dispatch로 처리하므로, alpha-mask 여부는 shader permutation이 아니라 draw data의 `AlphaMode` uniform branch로 선택합니다.

## Build-Time Optimization Note

현재 구현에서 `vertex_count = State.Dag.Positions.size()`는 누적 값(이전 레벨 포함 전체 합)입니다. 이 때문에 상위 레벨로 갈수록 `vertex_count`가 커지고, 레벨마다 다음 비용이 반복됩니다.

- meshopt ref 배열을 누적 정점 수 기준으로 할당
- canonical 맵을 레벨마다 전체 크기로 재빌드

결과적으로 레벨당 `O(전체 정점)` 비용이 발생하며, 이는 correctness와 무관하게 build time만 증가시킵니다.

추후 최적화 방향:

- "현재 레벨에서 실제로 참조되는 정점"으로 작업 범위를 좁혀,
- ref 배열/맵 빌드를 레벨 로컬 정점 집합 기준으로 제한

이 최적화는 출력 정합성을 바꾸지 않고 빌드 시간만 줄이는 성격입니다.
