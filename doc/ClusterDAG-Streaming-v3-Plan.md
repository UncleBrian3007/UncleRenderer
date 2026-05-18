# ClusterDAG Streaming v3 - 실제 page streaming 현황과 계획

## 문서 역할

이 문서는 `ClusterDAG-Streaming-v1.md`의 feedback/readback/scheduler 기반 위에 올라간 실제 page streaming 구현을 정리한다. v1/v2 문서는 초기 구조와 역사적 기준으로 유지하고, 이 문서는 `.vmesh` page payload를 디스크에서 읽어 `PageDataBuffer`에 업로드한 뒤 shader가 resident page를 사용하는 v3 계열의 현재 기준으로 둔다.

두 문서는 합치지 않는다. v1/v2는 기반 구조를 설명하고, v3 문서는 disk I/O, GPU upload, physical slot, paged shader fetch, 후속 계획을 추적한다. 하나로 합치면 완료된 기반, v3에서 대체된 임시 경로, 앞으로 남은 작업이 섞여 읽기 어려워진다.

## 현재 요약

| 버전 | 상태 | 핵심 |
|------|------|------|
| v3.0 | 구현됨 | streaming-ready `.vmesh` v16, page directory, 4 KB aligned payload |
| v3.1 | 구현됨 | requested page의 async disk read, I/O in-flight 제한 |
| v3.2 | 구현됨 | `PageDataBuffer` physical slot upload, fence 완료 후 resident |
| v3.3 | 구현됨 | resident non-root group/child-ref를 page slot에서 shader fetch |
| v3.4 | 구현됨 | cluster/draw/index/packed vertex payload를 page-local fetch |

현재 v3.4는 page-local geometry fetch까지 들어간 하이브리드 단계다. resident page payload는 group/child-ref뿐 아니라 scene-ready cluster record, draw data record, packed index payload, packed vertex stream을 포함한다. persistent traversal과 level-split traversal은 child cluster를 처리할 때 global `ClusterBuffer`보다 page-local cluster record를 우선 읽는다. visible candidate가 inline으로 처리되거나 candidate queue를 타는 경우 모두 page data base를 보존하며, visibility buffer path의 hardware visibility, software raster, resolve shader는 page-local index/packed vertex stream을 우선 읽는다. material texture와 scene/model constant, command template의 기본 저장소는 아직 기존 scene-wide/global buffer 경로를 유지한다.

## Runtime 흐름

```text
GPU traversal
  - non-resident page 발견
  - feedback UAV에 request 기록
  - 해당 branch refinement 중단

CPU feedback consume
  - frame fence signal 이후 readback 소비
  - duplicate request 병합
  - priority 기준 pending queue 유지

CPU I/O
  - pending page의 .vmesh page directory lookup
  - MaxIoInFlight 이하로 async disk read 발행
  - 완료된 payload를 IoComplete로 전환

GPU upload
  - IoComplete payload를 scene-ready GPU page payload로 변환
  - PageDataBuffer physical slot 할당
  - RenderGraph pass에서 CopyBufferRegion 기록
  - frame fence 완료 후 page table resident bit set

Shader paged fetch
  - resident non-root group은 PageTable.PhysicalPageIndex 확인
  - PageDataBuffer header 검증
  - group/child-ref/cluster/draw/index/packed vertex stream을 page slot에서 우선 load
```

## 주요 구현 기준점

- `Source/Scene/ClusterDAGBuilder.cpp`
  - `.vmesh` v16 cache 저장/로드
  - page directory 작성
  - asset-local streaming page payload 생성
  - debug roundtrip validation

- `Source/Render/Deferred/ClusterDagRuntime.cpp`
  - global streaming page index 할당
  - scene-ready streaming page source 구성
  - page-local cluster/draw metadata 생성
  - shader binding에 page table/page data 전달

- `Source/Render/Deferred/ClusterDagStreamingManager.cpp`
  - page table, feedback buffer, readback buffer
  - page read source 초기화
  - pending/read/upload/resident pipeline
  - physical slot pool
  - GPU page payload packing

- `Shaders/ClusterDag/ClusterDagTraversalCommon.hlsl`
  - residency check
  - streaming request 기록
  - paged group/child-ref/cluster/draw fetch helper

## v3.0 - Streaming-ready `.vmesh`

목표는 기존 순차 cache dump/restore 포맷을 page-addressable 포맷으로 바꾸는 것이다.

구현된 내용:

- `GVmeshVersion`을 16으로 올려 기존 cache를 무효화했다.
- cache header에 page directory offset/count를 포함한다.
- page payload file offset은 4 KB로 정렬한다.
- page directory entry는 payload 위치, 크기, mesh/dag/local page index, flag를 가진다.
- root/restore payload와 streaming payload를 directory에서 구분한다.
- public cache load/save API는 유지한다.
- debug build에서 save 후 reload한 결과를 원본 build 결과와 비교하는 roundtrip validation을 수행한다.
- streaming page payload가 `GClusterDAGVmeshStreamingPageSlotBytes`를 초과하면 build/cache save를 실패시킨다.

## v3.1 - CPU-side page I/O

목표는 GPU feedback으로 요청된 page payload를 디스크에서 CPU staging memory로 읽는 것이다.

구현된 내용:

- runtime이 global page index를 `.vmesh` cache path, source path, mesh index, primitive/dag index, local page index에 연결한다.
- streaming manager가 `.vmesh` page directory를 읽고 global page별 read source를 구성한다.
- `std::async(std::launch::async, ...)` 기반으로 page payload read를 발행한다.
- `ClusterDAGStreamingMaxIoInFlight`가 동시 read 수를 제한한다.
- duplicate request는 같은 pending/read/upload entry로 병합되어 중복 read를 발행하지 않는다.
- I/O issue/done/fail counter를 ImGui stats에 표시한다.

## v3.2 - `PageDataBuffer` slot pool과 GPU upload

목표는 staging payload를 GPU `PageDataBuffer` physical slot으로 복사하고, copy가 포함된 frame fence가 끝난 뒤에만 resident로 보이게 하는 것이다.

구현된 내용:

- `PageDataBuffer`를 fixed-size physical slot pool로 사용한다.
- runtime slot size는 `.vmesh` 포맷과 맞춘 `GClusterDAGVmeshStreamingPageSlotBytes`를 사용한다.
- root page는 slot pool에 예약된다.
- `IoComplete` page는 upload buffer를 만들고 physical slot을 할당받는다.
- RenderGraph begin-frame pass에서 `CopyBufferRegion`으로 payload를 `PageDataBuffer`에 복사한다.
- upload가 제출된 frame fence가 signal된 뒤에만 `MarkPageResident(PageIndex, SlotIndex, FrameNumber)`를 호출한다.
- `FClusterDagPageTableEntry::PhysicalPageIndex`는 logical page index가 아니라 physical slot index를 저장한다.
- upload byte cap은 `ClusterDAGStreamingMaxPageUploadBytesPerFrame`로 제한한다.
- payload validation 실패나 upload buffer 생성 실패는 해당 page만 fail 처리하고 다른 `IoComplete` page 처리는 계속한다.
- free slot이 부족하면 read 발행 전 slot budget을 고려해 불필요한 read를 줄인다.
- slot pool stats는 used/total/free와 slot-full drop을 표시한다.

## v3.3 - Physical slot 기반 group/child-ref fetch

목표는 resident page의 group과 child-ref를 shader가 `PageDataBuffer` physical slot에서 직접 읽는 것이다.

구현된 내용:

- persistent traversal과 level-split traversal 모두 page table/page data binding을 받는다.
- resident non-root group 처리 시 `PageTable[PageIndex].PhysicalPageIndex`를 통해 physical slot을 찾는다.
- page slot payload header를 검증한 뒤 group과 child-ref를 `ByteAddressBuffer PageData`에서 load한다.
- header mismatch, invalid slot, non-resident page는 기존 streaming request/fallback gate를 따른다.
- 임시 debug counter로 resident page에서 paged group fetch가 실제 발생하고 fallback/header fail이 없는 것을 확인한 뒤 counter 코드는 제거했다.

## v3.4 - Page-local geometry fetch

목표는 v3.3의 하이브리드 경로를 확장해 cluster, draw metadata, index, packed vertex stream까지 page-local data로 읽게 하는 것이다.

구현된 내용:

- GPU page payload header version을 3으로 올렸다.
- header에 group, child-ref, cluster record, draw data record, packed index, packed position/normal/uv/tangent/color offset/count를 넣었다.
- runtime이 upload 직전에 scene transform과 global remap이 반영된 scene-ready page payload를 만든다.
- page payload에는 다음 record가 들어간다.
  - group data
  - child refs
  - global cluster index + scene-space cluster data
  - global draw data index + draw metadata
  - packed index payload
  - packed position/normal/uv/tangent/color payload
- HLSL helper를 추가했다.
  - `TryLoadClusterDagPagedCluster()`
  - `TryLoadClusterDagPagedDrawData()`
  - `LoadClusterDagCluster()`
  - `LoadClusterDagDrawData()`
  - `LoadClusterDagPagedVertexIndex()`
  - `LoadClusterDagPagedPackedPositionWords()`
  - `LoadClusterDagPagedPackedScalar()`
- persistent traversal은 page-local cluster record를 우선 사용한다.
- persistent inline visible candidate 경로는 page-local draw metadata를 우선 사용한다.
- level-split node traversal은 page-local cluster record를 우선 사용한다.
- persistent candidate queue와 level-split candidate queue는 cluster index와 page data base를 함께 저장한다.
- visibility entry는 global draw data index와 page data base를 함께 기록한다.
- hardware visibility pass는 page-local index/packed position을 읽어 depth visibility를 작성한다.
- software raster pass는 page-local draw/index/packed position을 읽어 visibility buffer를 작성한다.
- resolve pass는 page-local draw/index/packed position/normal/uv/tangent/color를 읽어 GBuffer를 복원한다.
- hardware indirect command는 global command template을 복사한 뒤 draw index start를 page-local `StartIndex`로 patch한다.
- global cluster/draw/index/vertex buffers는 non-streaming fallback, root/coarse data, material/scene 상수 경로를 위해 유지한다.

검증 기준:

- page-local cluster fetch와 global cluster fetch의 visible cluster count, draw count, mip histogram이 일치한다.
- persistent queue와 level-split queue 모두 통과한다.
- force mip/debug view에서 global path와 page-local path의 시각 결과가 일치한다.
- page-local index/vertex fetch를 켠 상태에서 visibility buffer path의 depth visibility, software raster, resolve 결과가 global path와 일치한다.

## 남은 미비점과 후속 개선

### Upload staging ring buffer

현재는 page payload마다 `CreateUploadBuffer()`로 별도 upload committed resource를 만든다. `FGpuUploadingPage`가 fence 완료까지 upload buffer를 보유하므로 수명 관리는 맞다. 다만 cold load나 빠른 카메라 이동처럼 수백~수천 page가 몰리는 상황에서는 committed resource 생성/파괴 비용이 커질 수 있다.

후속 개선:

- frame/fence indexed upload arena 또는 ring buffer를 만든다.
- page payload마다 committed resource를 만들지 않고 ring range를 suballocate한다.
- `FGpuUploadingPage`는 upload resource 대신 `{ Offset, Size, FenceValue }`를 추적한다.
- ring capacity 부족과 `MaxPageUploadBytesPerFrame` cap을 같은 scheduling 경로에서 처리한다.

### I/O retry와 backoff

현재 read 실패는 해당 page를 failed 처리하고 counter만 증가시킨다. 계획 범위에는 맞지만 transient failure 회복은 없다.

후속 개선:

- 파일 없음, version mismatch, payload size 오류 같은 permanent failure와 transient failure를 분리한다.
- page별 retry count와 next retry frame/time을 둔다.
- 같은 page가 매 frame request되어도 즉시 read를 반복하지 않도록 backoff한다.
- retry exhausted 상태를 stats/debug에 노출한다.

### Eviction

현재 eviction은 없다. slot pool이 가득 차면 새 page는 resident가 되지 못하고 slot-full drop으로 남는다. read 발행 전 free slot budget을 고려해 헛읽기는 줄였지만, 장시간 이동하는 scene에서는 결국 새 page를 받을 수 없는 상태가 된다.

후속 개선:

- `LastUsedFrame`, priority, visibility를 eviction score로 사용한다.
- root page와 필수 page는 eviction 대상에서 제외한다.
- GPU가 접근할 수 있는 page는 frame fence 이후에만 free한다.
- evicted page는 resident bit를 내리고 physical slot을 free list로 반환한다.

### Page-local path 범위 확장

v3.4에서 visibility buffer path는 page-local geometry fetch를 사용한다. 아직 scene-wide/global 경로가 완전히 사라진 것은 아니다.

후속 개선:

- material texture, scene/model constant, command template 저장소를 어느 범위까지 page-local payload와 분리할지 정리한다.
- non-visibility fallback path가 필요한 경우 page-local geometry helper를 공유하도록 확장한다.
- page payload layout을 더 압축하거나 page-local packed stream 중심으로 재배치한다.

### ClusterDAG global geometry buffer 제거 조건

목표는 ClusterDAG 전용 global packed vertex/index buffer를 없애고, resident page의 `PageDataBuffer` payload만 visibility/resolve geometry source로 쓰는 것이다. 여기서 제거 대상은 `FSceneModelResource::ClusterDagVertexBuffers`, `ClusterDagIndexBuffer`, `ClusterDagColorBuffer`처럼 ClusterDAG runtime을 위해 별도로 만든 packed geometry buffer다. 일반 mesh rendering, ray tracing, object-id 같은 경로가 쓰는 `Model.Geometry` vertex/index buffer는 별도 대상이다.

현재 남아 있는 의존성:

- `SceneModelResourceLoader.cpp`는 ClusterDAG runtime 모델에 대해 `ClusterDagPosition/Normal/UV/Tangent/ColorBuffer`와 `ClusterDagIndexBuffer`를 만들고, 이 생성이 실패하면 `bUseClusterDagRuntime`을 끈다.
- `RendererUtils.cpp`는 `bUseClusterDagIndexBuffer`가 true일 때 scene constants의 `VertexBufferBindlessIndices`와 `ExtraBindlessIndices.y`에 ClusterDAG 전용 global packed buffer를 바인딩한다.
- `DeferredBasePass.cpp`는 `ClusterDagVisibilityPass`가 준비되지 않았을 때 ClusterDAG indirect draw를 직접 실행하는 fallback path를 갖고 있다. 이때 `DeferredBasePass.hlsl`은 global index/vertex buffer를 읽는다.
- `ClusterDagVisibility.hlsl`과 `ClusterDagGeometryFetch.hlsl`에는 page-local fetch 실패 시 global index/vertex buffer로 되돌아가는 fallback이 남아 있다.
- debug view는 `ClusterDagDebugColorBuffer`를 통해 global debug color table을 참조한다.

제거 전제:

- visibility buffer path를 ClusterDAG runtime의 필수 경로로 만들거나, DeferredBasePass fallback path도 page-local geometry fetch를 사용하도록 바꾼다.
- non-root streamed page는 항상 packed index, position, normal, uv, tangent, color payload를 포함하도록 upload 전 검증한다.
- streamed visible entry는 항상 valid `PageDataBase`를 갖도록 traversal/candidate queue 계약을 고정한다.
- root/coarse page 정책을 정한다. 안전한 1차 목표는 root만 global fallback을 허용하고 non-root resident page는 page-local only로 강제하는 것이다.
- page-local fetch fail/header fail counter를 추가하고, 강제 mip/debug view에서 fallback 없이 0 fail을 확인한다.

작업 순서:

1. payload 완전성 검증을 강화하고 누락 stream page는 upload 실패로 처리한다.
2. non-root streamed visible entry에서 shader global fallback을 제거한다.
3. `ClusterDagVisibilityPass` required 정책 또는 DeferredBasePass page-local fallback 중 하나를 선택한다.
4. ClusterDAG scene constants에서 global packed vertex/index bindless 의존을 제거한다.
5. loader에서 `ClusterDagVertexBuffers`, `ClusterDagIndexBuffer`, `ClusterDagColorBuffer` 생성을 선택 사항으로 낮춘 뒤, 참조가 사라지면 제거한다.
6. debug color도 page-local record 또는 별도 lightweight debug table로 옮긴다.

완료 기준:

- `Shaders/ClusterDagVisibility.hlsl`, `Shaders/ClusterDag/ClusterDagGeometryFetch.hlsl`, `Shaders/DeferredBasePass.hlsl`의 ClusterDAG runtime 경로가 `ClusterDagVertexBuffers`/`ClusterDagIndexBuffer`에 의존하지 않는다.
- ClusterDAG visibility/resolve 결과가 기존 global path와 일치한다.
- streaming page-local fetch fail/header fail counter가 정상 scene에서 0이다.
- `SceneModelResourceLoader.cpp`에서 ClusterDAG runtime enable 여부가 global packed vertex/index GPU buffer 생성 성공 여부에 의존하지 않는다.

### Stats와 검증 보강

필요하면 정식 debug stats로 다음 항목을 추가한다.

- total request / duplicate merged
- total read issued / completed / failed
- total upload issued / completed / bytes
- page-local fetch success / fallback / header fail
- retry scheduled / exhausted
- eviction count / eviction stall

## Config와 UI

현재 관련 config:

```ini
[ClusterDAG]
bEnableClusterDAGStreaming=true
ClusterDAGStreamingPoolMB=256
ClusterDAGStreamingRequestBufferCapacity=65536
ClusterDAGStreamingMaxPendingPages=64
ClusterDAGStreamingMaxPageInstallsPerFrame=16
ClusterDAGStreamingPageSlotBytes=131072
ClusterDAGStreamingMaxIoInFlight=8
ClusterDAGStreamingMaxPageUploadBytesPerFrame=8388608
```

주의:

- `ClusterDAGStreamingPageSlotBytes`는 config에 남아 있지만 runtime은 fixed `.vmesh` slot size인 `GClusterDAGVmeshStreamingPageSlotBytes`를 사용한다.
- ImGui는 page slot size를 KB 단위 read-only 값으로 보여준다.
- max upload budget은 MB 단위로 조절한다.

주요 stats:

- `Stream Pages R/P/I`: resident / pending / installed
- `Stream Req/Drop/Repl`: request / dropped / priority replacement
- `Stream IO F/Iss/Done/Fail`: in-flight / issued / completed / failed
- `Stream Upload F/Iss/Done/MB`: uploading / issued / completed / uploaded MB
- `Stream Slots U/T/F`: used / total / free
- `Stream Slot KB/Full Drops`: slot size / slot-full drop count

## 완료 기준

현재 완료:

- `.vmesh` page directory 기반 streaming payload 저장/로드
- debug roundtrip validation
- GPU feedback request -> CPU readback -> pending scheduler
- async disk read와 in-flight 제한
- `PageDataBuffer` physical slot upload
- upload fence 완료 후 resident bit visibility
- resident non-root group/child-ref의 shader-side paged fetch
- resident page의 cluster/draw metadata page-local fetch
- visibility buffer path의 page-local index/packed vertex fetch

아직 남음:

- upload staging ring buffer
- retry/backoff
- eviction
- 정식 page-local fetch debug stats

## 참고 파일

- `doc/ClusterDAG-Streaming-v1.md`
- `Source/Scene/ClusterDAG.h`
- `Source/Scene/ClusterDAGBuilder.cpp`
- `Source/Render/Deferred/ClusterDagStreamingManager.h`
- `Source/Render/Deferred/ClusterDagStreamingManager.cpp`
- `Source/Render/Deferred/ClusterDagRuntime.cpp`
- `Shaders/ClusterDag/ClusterDagTraversalCommon.hlsl`
- `Shaders/ClusterDag/PersistentClusterDagCull.hlsl`
- `Shaders/ClusterDag/ClusterDagLevelSplitNodeCull.hlsl`
