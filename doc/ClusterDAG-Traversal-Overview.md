# ClusterDAG Traversal 개요

## 목적

이 문서는 UncleRenderer에 현재 구현된 ClusterDAG GPU traversal 작업을 요약합니다.

builder, visibility buffer, streaming 구현은 의도적으로 깊게 다루지 않습니다. 해당 영역은 아래의 동반 문서를 참고하세요.

- `doc/ClusterDAG-Builder-Overview.md`
- `doc/ClusterDAG-VisibilityBuffer-v1.md`
- `doc/ClusterDAG-Streaming-v1.md`
- `doc/ClusterDAG-Streaming-v3-Plan.md`

이 문서의 초점은 다음 파일들입니다.

- `Source/Render/Deferred/ClusterDagRuntime.cpp`
- `Source/Render/Deferred/ClusterDagRuntime.h`
- `Shaders/ClusterDag/InitClusterDagQueues.hlsl`
- `Shaders/ClusterDag/PersistentClusterDagCull.hlsl`
- `Shaders/ClusterDag/InitClusterDagLevelSplitQueues.hlsl`
- `Shaders/ClusterDag/PrepareClusterDagLevelSplitNodeArgs.hlsl`
- `Shaders/ClusterDag/ClusterDagLevelSplitNodeCull.hlsl`
- `Shaders/ClusterDag/PrepareClusterDagLevelSplitClusterArgs.hlsl`
- `Shaders/ClusterDag/ClusterDagLevelSplitClusterCull.hlsl`
- `Shaders/ClusterDag/ClusterDagTraversalCommon.hlsl`

## Runtime 데이터 계약

Traversal은 root cluster가 아니라 root group에서 시작합니다.

builder는 `FRuntimeClusterHierarchy`를 제공하고, runtime upload 단계에서 이를 GPU 버퍼들로 변환합니다.

- group buffer
- cluster buffer
- child-ref buffer
- root-group buffer
- draw-data buffer
- indirect command template buffer

중요 개념은 다음과 같습니다.

- `Group`은 hierarchy node입니다.
- `Cluster`는 실제 렌더 가능한 단위입니다.
- `Cluster.GeneratingGroupIndex`는 cluster가 refine되어야 할 때 사용할 group을 가리킵니다.
- `Group.ChildRefs`는 child cluster들을 가리킵니다.
- `DrawData` 레코드는 visible cluster를 material/range command slot과 연결합니다.

이 계약이 예전의 root-cluster 단위 DFS 가정을 제거한 핵심 이유입니다.

## 현재 Traversal 모드

현재 config enum:

- `PersistentQueue`
- `LevelSplitQueue`

`FRendererConfig`의 기본값은 `LevelSplitQueue`입니다.

다음 alias 키들에서 config를 파싱합니다.

- `ClusterDAGTraversalMode`
- `ClusterDAGTraversal`
- `ClusterDAGMode`

지원 값:

- `PersistentQueue`, `persistent_queue`, `persistent`
- `LevelSplitQueue`, `level_split_queue`, `levelsplit`, `level_split`

## PersistentQueue 모드

`PersistentQueue`는 compact persistent traversal 경로입니다.

Frame graph 흐름:

1. Streaming begin-frame pass.
2. `Init ClusterDAG Queues`.
3. `Persistent ClusterDAG Cull`.
4. Streaming feedback readback pass.
5. `Finalize ClusterDAG Indirect Args`.

init pass는 root group을 group queue에 시드하고, counter와 run count를 초기화하며, traversal epoch 상태를 설정합니다.

persistent cull shader는 다음 작업을 처리합니다.

- group queue work
- candidate cluster queue work

Group 작업:

- group 로드
- child ref 순회
- child cluster visibility와 LOD 평가
- refinement가 필요하면 generating group enqueue
- 렌더링이 필요하면 candidate cluster enqueue

Candidate cluster 작업:

- cluster draw-data 레코드 로드
- range별 indirect command append
- range별 run count 증가
- 이후 visibility-buffer에서 사용할 visible-entry 메타데이터 기록

Persistent 모드는 pass 수를 작게 유지한다는 장점이 있습니다. 반면 단일 compute shader 내부에서 queue 진행과 종료 동작이 더 복잡해진다는 단점이 있습니다.

## LevelSplitQueue 모드

`LevelSplitQueue`는 hierarchy node 처리와 최종 cluster emission을 분리합니다.

Frame graph 흐름:

1. Streaming begin-frame pass.
2. `Level Split ClusterDAG Init`.
3. 각 `RuntimeMaxTraversalLevels`에 대해:
   - `Level Split ClusterDAG Prepare Node`
   - `Level Split ClusterDAG Node Cull`
4. `Level Split ClusterDAG Prepare Cluster`
5. `Level Split ClusterDAG Cluster Cull`
6. Streaming feedback readback pass.
7. `Level Split ClusterDAG Finalize Indirect Args`.

Node cull pass:

- 후보 group 하나를 thread group 하나가 처리
- group thread들이 child ref를 스캔
- 다음 레벨 group을 다음 ping-pong node candidate buffer에 기록
- terminal cluster를 candidate cluster buffer에 기록
- visited group epoch buffer로 group enqueue를 중복 제거

Cluster cull pass:

- candidate cluster 소비
- indirect draw command 생성
- run count 갱신
- visible-entry 메타데이터 기록

이 모드는 persistent queue 경로보다 추론하기 쉽고, node/cluster 분리 culling 구조를 따릅니다. 대신 traversal 깊이가 증가할수록 pass 수가 늘어나는 tradeoff가 있습니다.

## Queue 및 Counter 리소스

중요한 per-frame 버퍼:

- `QueueStateBuffers`
- `GroupQueueBuffers`
- `CandidateClusterEntryBuffers`
- `VisitedGroupEpochBuffers`
- `LevelSplitNodeCandidateBuffers[2]`
- `LevelSplitNodeArgsBuffers[2]`
- `LevelSplitClusterArgsBuffers`
- `IndirectCommandBuffers`
- `RunCountBuffers`
- `VisibleEntryBuffers`
- `VisibleEntryCounterBuffers`
- `HwVisibleEntryIndexBuffers`
- `SwVisibleEntryIndexBuffers`
- `DrawDataVisibleEntryIndexBuffers`

traversal epoch는 매 프레임 visited-group 상태를 클리어하지 않기 위해 사용됩니다. CPU는 frame 기반 epoch를 증가시키고, shader는 epoch buffer를 사용해 해당 프레임 내 group enqueue를 deduplicate합니다.

## Visible 출력

Traversal은 두 종류의 출력을 생성합니다.

Draw 출력:

- indirect command buffer
- per-range run-count buffer

Visibility 메타데이터 출력:

- visible entries
- visible-entry counters
- hardware visible-entry index list
- software visible-entry index list
- draw-data-to-visible-entry index list

draw 출력은 indirect drawing에 사용되고, visibility 메타데이터는 visibility-buffer 경로와 software raster 경로에 사용됩니다.

## LOD 결정

현재 traversal이 지원하는 항목:

- `ClusterDAGTargetErrorPixels` 기반 projected LOD error
- `ClusterDAGForceMip` 기반 force-mip 디버깅
- `ClusterDAGForceSoftwareRaster` 기반 software raster 강제
- `ClusterDAGSwRasterThresholdPixels` 기반 software raster 임계값

`ForceMip`은 subtree 누락 없이 traversal이 leaf-level 출력까지 도달하는지 점검할 때 특히 유용합니다.

## Fast/Debug Shader 퍼뮤테이션

Traversal shader는 4가지 select permutation으로 컴파일됩니다.

- default
- debug
- fast
- fast debug

debug permutation은 추가 counter와 선택적 debug line을 기록합니다.

fast permutation은 일부 방어적 shader 체크를 제거합니다. builder/runtime validation으로 GPU 데이터 신뢰성이 이미 확보된 경우에만 사용해야 합니다.

## Render Graph 통합

ClusterDAG traversal은 import된 persistent buffer에 대해 render graph external buffer tracking을 사용합니다.

pass setup에서는 다음을 선언합니다.

- read buffers
- write buffers
- indirect argument buffers
- 필요한 경우 UAV barrier

이 구조 덕분에 pass setup이 리소스 사용을 소유하고, dispatch helper는 상수 바인딩과 dispatch/execute-indirect 호출에 집중할 수 있습니다.

## 다음 Traversal 작업

### Correctness

- traversal validation을 builder validation과 계속 연동합니다. builder/cache validator가 데이터 정합성을 입증한 뒤에만 runtime guard를 줄여야 합니다.
- 모든 `GeneratingGroupIndex`가 유효하거나 의도적으로 invalid인지 명시적으로 검증합니다.
- 모든 root group이 최소 하나의 renderable cluster에 도달하는지 검증을 추가합니다.
- 누락된 candidate cluster와 capacity 초과 command append를 위한 debug counter를 추가합니다.

### Performance

- 동일한 씬에서 `PersistentQueue`와 `LevelSplitQueue`를 프로파일링합니다.
- `RuntimeMaxTraversalLevels`에 따른 pass 오버헤드를 측정합니다.
- queue high-water mark와 buffer capacity를 함께 추적합니다.
- 두 traversal 모드를 장기적으로 모두 유지할지 결정합니다.

### Debuggability

- 간결한 traversal 통계 출력 추가:
  - 총 visible cluster 수
  - group queue peak
  - candidate queue peak
  - dedup count
  - overflow count
  - software raster candidate count
- 새 에셋에 대해 `ForceMip` 테스트를 정기 debug 체크리스트에 포함합니다.
- 생성된 group 전이 또는 선택된 mip level을 볼 수 있는 시각적 debug 모드를 추가합니다.

## 권장 단기 순서

1. queue peak, overflow, visible cluster, candidate cluster 통계를 먼저 추가합니다.
2. builder validation이 자리 잡은 뒤 SciFiHelmet과 Duck에 대해 `ForceMip=0` 체크를 실행합니다.
3. GPU 타이밍을 켠 상태에서 `PersistentQueue`와 `LevelSplitQueue`를 프로파일링합니다.
4. fast shader가 더 강한 validated-cache 플래그를 요구해야 하는지 결정합니다.
5. 현재 traversal 출력이 안정화된 뒤 post-occlusion traversal split을 다시 검토합니다.
