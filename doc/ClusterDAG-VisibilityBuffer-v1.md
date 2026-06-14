# ClusterDAG Visibility Buffer v1

## 요약

이 문서는 UncleRenderer에 현재까지 구현된 ClusterDAG visibility-buffer 경로를 정리합니다. 왜 이 경로를 도입했는지, deferred renderer에 어떻게 연결되어 있는지, 현재 동작하는 부분과 남은 작업은 무엇인지에 초점을 둡니다.

현재 구현은 하이브리드 경로입니다.

- `ClusterDAG` 런타임 지오메트리는 전용 visibility-buffer 경로로 렌더링됩니다.
- `ClusterDAG`가 아닌 메시는 기존 legacy deferred base pass를 계속 사용합니다.
- visibility 경로의 현재 목표는 순수한 RT 대역폭 절감보다 correctness와 overdraw 감소를 우선하는 것입니다.

따라서 이 구현은 material classification 파이프라인보다는, "하드웨어 raster visibility buffer + fullscreen resolve" 단계에 더 가깝습니다.

## 목표

이 작업의 원래 목표는 다음과 같았습니다.

- 가려진 `ClusterDAG` 삼각형에 대해 전체 material shading 비용을 지불하지 않기
- `ClusterDAG` 렌더링을 visibility-buffer 모델에 더 가깝게 이동하기
- 표준 `GBuffer`를 재구성해 기존 deferred lighting 파이프라인을 유지하기
- 첫 구현 범위를 기존 `ClusterDAG runtime` 경로로 제한해 마이그레이션 리스크를 낮추기

현재 구현은 두 번째, 세 번째 목표를 달성했고, 첫 번째 목표는 부분적으로 달성했습니다.

## 현재 프레임 흐름

visibility-buffer 경로가 활성화되고 준비된 경우, deferred 프레임에서 `ClusterDAG`는 다음 순서로 처리됩니다.

1. `ClusterDagRuntime`
   - 순회(traverse), culling, visible cluster draw command 생성
2. `ClusterDagVisibilityPass`
   - hardware visibility raster pass
   - SW raster fallback pass
   - depth export pass
   - fullscreen resolve pass
3. legacy deferred base pass
   - `ClusterDAG`가 아닌 메시는 계속 렌더링
   - visibility resolve가 활성화되면 `ClusterDAG` 직접 base-pass shading은 건너뜀
4. deferred lighting / SSR / GTAO / post pass들

관련 코드:

- `Source/Render/Deferred/ClusterDagRuntime.*`
- `Source/Render/Deferred/ClusterDagVisibilityPass.*`
- `Shaders/ClusterDagVisibility.hlsl`
- `Shaders/ClusterDagResolve.hlsl`
- `Shaders/ClusterDagDepthExport.hlsl`
- `Source/Render/Deferred/DeferredFrameOrchestrator.cpp`
- `Source/Render/Deferred/DeferredBasePass.cpp`

## 구현된 구성 요소

### 1. 전용 Deferred 서브모듈

전용 deferred 서브모듈이 추가되었습니다.

- `FClusterDagVisibilityPass`

이 서브모듈은 다음을 소유합니다.

- visibility root signature 및 PSO
- software-raster root signature 및 PSO
- depth-export root signature 및 PSO
- resolve root signature 및 PSO
- visibility command signature
- persistent visibility 리소스
- deferred frame graph에 pass 등록

이 구조 덕분에 새 경로를 `FDeferredBasePass`에 섞지 않아도 되고, 전역 상태를 `FRenderer`에 더 밀어 넣는 것도 피할 수 있습니다.

### 2. Persistent Visibility 리소스

현재 경로는 persistent `Vis64` 텍스처를 사용합니다.

- `DXGI_FORMAT_R32G32_UINT`

`uint64` payload에는 다음이 저장됩니다.

- 상위 32비트: packed depth
- 하위 32비트: `(visibleEntryIndex + 1) << 7 | primitiveId`

이 인코딩은 hardware visibility pass와 SW raster fallback pass에서 공통으로 사용됩니다.

### 3. Hardware Visibility Raster

`Shaders/ClusterDagVisibility.hlsl`이 hardware raster visibility pass를 구현합니다.

이 pass는 다음을 수행합니다.

- indirect draw를 통해 visible `ClusterDAG` cluster를 렌더
- color RT는 기록하지 않음
- 일반 depth 기록
- `Vis64` UAV에 `InterlockedMax`로 packed visibility 기록

depth 규약은 렌더러의 reverse-Z 투영과 일치합니다.

- reverse-Z 투영은 `Source/Scene/Camera.cpp`에 정의
- visibility hardware PSO는 `GREATER_EQUAL` 사용
- `InterlockedMax`로 가장 가까운 표면을 유지

### 4. SW Raster Fallback

해당 서브모듈에는 visible entry를 처리하는 SW raster fallback 경로도 포함되어 있습니다.

이 경로는 다음을 수행합니다.

- dispatch args 준비
- 동일한 `Vis64` 텍스처에 visibility rasterize
- 선택적으로 HZB rejection 사용

즉 HW visibility 경로와 SW raster fallback 경로는 동일한 visibility 인코딩과 동일한 resolve 경로로 합류합니다.

### 5. Depth Export

visibility 기록 이후 전용 depth-export pass가 `Vis64`에서 depth를 복원해 scene depth buffer로 다시 기록합니다.

덕분에 `ClusterDAG` visibility가 먼저 UAV 기반 표현으로 기록되더라도, 이후 pass들은 표준 deferred depth를 그대로 사용할 수 있습니다.

### 6. Fullscreen Resolve

`Shaders/ClusterDagResolve.hlsl`는 visible `ClusterDAG` 픽셀의 최종 `GBuffer`와 emissive 출력을 재구성합니다.

resolve pass는 다음을 수행합니다.

- `Vis64` 읽기
- `visibleEntryIndex`와 `primitiveId` 해석
- `ClusterDagDrawData` 조회
- 필요 시 `PageData`에서 paged draw data 해석
- triangle index와 packed vertex data fetch
- position, normal, tangent, color, UV, material index 재구성
- material feature 평가
- 다음 출력 기록:
  - `GBufferA`
  - `GBufferB`
  - `GBufferC`
  - `GBufferD`
  - `SceneColor` / emissive target

이 단계가 deferred lighting 파이프라인 나머지를 그대로 유지할 수 있게 해주는 핵심 호환 브리지입니다.

### 7. Resolve용 ClusterDAG Scene 데이터

resolve를 지원하기 위해 `ClusterDAG` 전용 scene/material 데이터가 준비되었고, `drawData.ModelIndex`로 인덱싱 가능한 형태로 노출됩니다.

현재 runtime은 resolve가 다음 정보를 복원할 수 있을 만큼의 데이터를 제공합니다.

- model index
- material texture index
- material feature pipeline key
- transform
- paged fetch metadata

또한 `ClusterDAG` 경로의 scene constant 준비는 중복 제거되어, 패스마다 동일한 per-model 데이터를 독립적으로 다시 만들지 않도록 정리되었습니다.

### 8. Base Pass Fallback 제어

visibility resolve가 활성화되면 legacy deferred base pass는 더 이상 `ClusterDAG` 지오메트리를 `GBuffer`로 직접 셰이딩하지 않습니다.

이 점이 중요한 이유는, 그렇지 않으면 렌더러가 다음처럼 중복 작업을 하게 되기 때문입니다.

- legacy base pass에서 `ClusterDAG`를 한 번 셰이딩
- fullscreen resolve pass에서 결과를 다시 덮어씀

이는 작업 낭비를 만들고 shading mismatch도 유발할 수 있습니다.

### 9. 현재 제어 방식 (코드 기준)

현재 코드에는 `EnableClusterDAGVisibilityBuffer` 전용 토글과 `FClusterDagVisibilityPass::SetEnabled` 경로가 없습니다.

현재 동작은 다음과 같습니다.

- `FClusterDagVisibilityPass`는 별도 on/off 토글 없이 준비 상태(`IsReady`)와 런타임 경로 준비 상태로 사용 여부가 결정됩니다.
- `deferred renderer apply-config`에서 visibility pass에 적용되는 항목은 현재 `SetSoftwareRasterHzbRejectEnabled`입니다.
- `ClusterDAG` runtime 경로가 준비되지 않으면 base pass 경로에서 `ClusterDAG` runtime draw를 건너뛰는 대신 legacy base-pass fallback 경로가 동작합니다.

## 이 구현이 달성한 것

### Overdraw 감소

visibility pass는 가시성만 결정하고, 전체 material 평가는 resolve 단계로 지연됩니다. 따라서 가려진 `ClusterDAG` 삼각형은 base-pass 전체 material 비용을 지불하지 않습니다.

그 결과 다음 항목에서 낭비 작업이 줄어듭니다.

- normal map
- base color texture
- metallic-roughness texture
- emissive texture
- sheen / clearcoat / anisotropy 분기

다만 이 "material-shader overdraw 제거" 자체는 visibility buffer만의 고유 이득이 아닙니다. depth prepass 후 base pass를 `EQUAL` depth test로 돌리는 z-prepass로도 보이는 픽셀만 한 번 셰이딩되므로 동일하게 달성됩니다. 큰 삼각형 위주 장면에서는 둘이 사실상 동등합니다.

이 경로가 z-prepass 대비 실제로 다른 지점은 material overdraw가 아니라 다음 두 가지입니다.

- **지오메트리 1회 래스터**: z-prepass는 지오메트리를 depth pass와 base pass에서 두 번 래스터하지만, 이 경로는 vis pass에서 한 번만 래스터(색 없이 depth + visibility ID만 기록)하고 resolve는 fullscreen pass라 지오메트리를 다시 돌리지 않습니다. `ClusterDAG`처럼 삼각형 밀도가 높은 지오메트리에서는 이 vertex/raster 비용 차이가 큽니다.
- **Micro-triangle quad 효율**: z-prepass의 base pass도 2x2 quad로 셰이딩하므로 픽셀보다 작은 삼각형에서 helper-lane이 낭비됩니다. 이 경로는 셰이딩을 래스터에서 분리해 픽셀 단위로 한 번 셰이딩하므로 그 낭비를 피할 수 있습니다(아래 Micro-triangle 효율 항목 참고).

### Micro-triangle 효율 (조건부)

`ClusterDAG`에서는 작은 projected edge 길이의 cluster를 SW raster fallback으로 라우팅할 수 있습니다(`ClusterDAGSwRasterThresholdPixels`).

이 경우 fixed-function HW raster의 2x2 quad/helper-lane 비용을 직접 지불하지 않는 compute 기반 처리 경로를 타므로, micro-triangle 구간에서 효율 이득이 발생할 수 있습니다.

다만 이 이득은 항상 전체 경로에 적용되는 것은 아닙니다.

- 조건을 만족해 SW raster fallback으로 분기된 cluster에만 해당
- HW visibility로 남는 cluster는 기존 HW raster 특성을 그대로 가짐

### Deferred 파이프라인 호환성

렌더러는 최종적으로 표준 deferred `GBuffer`를 얻으므로, deferred 파이프라인 나머지는 큰 재작성 없이 계속 동작할 수 있습니다.

여기에는 다음이 포함됩니다.

- deferred direct lighting
- SSR
- GTAO
- 이후 post-processing pass

### Page-Local 지오메트리 Resolve

resolve 경로는 이미 `PageData`에서 page-local 지오메트리 데이터를 읽을 수 있습니다. 따라서 visibility 경로는 전역 packed geometry buffer에만 의존하지 않고, 더 넓은 `ClusterDAG` streaming/runtime 작업 흐름에 참여합니다.

## 이 구현이 아직 달성하지 못한 것

### 1. Render-Target 대역폭 절감

현재 설계는 RT 대역폭 절감 경로가 아닙니다.

지금의 `ClusterDAG` 경로는 다음을 수행합니다.

- visibility write
- depth export
- fullscreen `GBuffer` 재구성

따라서 일반 deferred base pass와 비교했을 때 RT 트래픽 자체를 줄이지는 않습니다. 이는 호환성과 hidden-surface shading 절감을 위한 의도적 trade-off이며, 현재의 주된 이득은 대역폭이 아닙니다.

### 2. Material Classification / Binning

현재 resolve 단계는 단일 fullscreen pass + 단일 uber-shader PSO 구조입니다.

material feature는 visible pixel을 material bucket으로 미리 분류하지 않고, `pipelineKey` 비트 기반 동적 분기로 활성화됩니다.

결과적으로 다음 문제가 발생합니다.

- 레지스터 압력 증가
- 특화 material shader 대비 occupancy 저하
- 혼합 material 픽셀 쿼드/웨이브 내 branch divergence
- per-pixel로 resolve된 `sceneData`/material descriptor 인덱스가 non-uniform이라 `NonUniformResourceIndex`가 필요하고, 이는 wave scalarization 비용을 유발 (`ClusterDAG-AMD-NonUniform-Bindless-Bug.md` 참고)

즉 현재 구현은 완전한 분류 기반 visibility-buffer 렌더러보다, 실질적으로 "fullscreen deferred uber resolve"에 더 가깝습니다.

### 3. MSAA 통합

현재 경로는 `SampleDesc.Count = 1`을 사용하며, MSAA 인지 visibility 저장/resolve를 지원하지 않습니다.

## 알려진 이슈와 리스크

### 1. 클러스터당 128 Triangle Visibility 인코딩 한계

현재 visibility 인코딩은 `primitiveId`에 7비트만 할당합니다.

- `primitiveId < 128`

hardware visibility shader와 SW raster fallback 경로 모두 이 제한을 가정합니다.

현재 동작:

- hardware 경로는 `PrimitiveId >= 128`이면 픽셀을 드롭
- software 경로는 `triangleCount`를 `128`로 클램프

builder/runtime이 이보다 큰 클러스터를 방출하면 픽셀이 조용히 사라질 수 있습니다.

따라서 build/runtime validation에서 반드시 명시적으로 보호해야 합니다.

### 2. Fullscreen Uber Resolve 비용

현재 resolve pass는 의도적으로 단순하지만, 대부분의 material feature가 비활성인 경우에도 모든 visible `ClusterDAG` 픽셀이 큰 resolve shader를 공통으로 실행합니다.

v1 단계에서는 허용 가능하지만 장기적인 성능 구조로는 이상적이지 않습니다.

### 3. 추가 Depth 처리 복잡도

visibility를 최종 scene depth와 분리해 저장하기 때문에, 경로에 depth-export 단계가 하나 더 필요합니다. 이는 하이브리드 설계의 비용이며 성능 분석 시 항상 고려해야 합니다.

### 4. SW Raster Near-Plane 교차

HW visibility 경로는 near plane을 가로지르는 triangle을 fixed-function clipping으로 처리하지만, SW raster fallback 경로는 현재 이 triangle을 클리핑하지 않습니다.

현재 동작:

- SW raster shader는 clip-space `w` 중 하나라도 near plane 뒤에 있으면 triangle을 거부
- 클리핑 후 부분적으로 보여야 하는 triangle 픽셀이 누락될 수 있음
- SW raster fallback 경로의 `ClipToPixel()` helper는 proper near-plane clipping의 대체 수단이 될 수 없음

권장되는 단기 대응은 near-plane 교차 triangle을 SW raster fallback으로 처리하려고 하지 말고 HW visibility 경로로 되돌려 보내는 것입니다.

## 권장 후속 작업

다음 작업은 아래 순서로 우선순위를 두는 것을 권장합니다.

### Priority 1 - Resolve 텍스처 Gradient 품질 검증

`SampleGrad()` 전환과 해석적 UV gradient 계산은 반영되었습니다. 이제 우선순위는 구현이 아니라 검증/튜닝입니다.

권장 접근:

1. 에지 밀집 장면, micro-triangle 장면, 고주파 텍스처 장면을 고정 카메라/패닝 카메라로 캡처
2. resolve 결과에서 mip flicker, seam 튐, normal-map aliasing을 체크
3. 필요 시 gradient clamp 또는 material별 bias 정책을 추가
4. SW raster fallback 경로와 HW visibility 경로의 시각적 일관성을 비교

가장 가치가 높은 후속 품질 작업입니다.

### Priority 2 - 128-Triangle 제한 강제

visibility 인코딩 계약이 조용히 깨지지 않도록 명시적 validation을 추가합니다.

권장 작업:

- builder에서 visibility cluster가 `128` triangle을 넘지 않도록 assert/validation 추가
- 필요 시 import된 cluster payload에 대해 runtime validation 추가
- visibility packing 코드 옆에 인코딩 가정 문서화

향후 더 큰 클러스터를 지원하려면 visibility 인코딩 자체를 변경해야 합니다.

### Priority 3 - SW Near-Plane 교차를 HW Raster로 우회

SW raster fallback 경로가 near plane 교차 triangle을 조용히 드롭하지 않게 해야 합니다.

권장 작업:

- SW raster 리스트를 만들기 전/중 near-plane 교차 entry 또는 triangle 감지
- 해당 entry를 SW raster dispatch에서 제외
- fixed-function clipper가 처리하도록 HW visibility 경로로 유지/우회
- 실제 SW clipping 구현이 추가되기 전까지는, SW raster를 near plane 앞에 안전하게 있는 triangle로 제한

이 방식은 v1 SW raster fallback 경로에서 복잡한 clipping 로직 중복을 피하면서도 근거리 지오메트리 correctness를 지킬 수 있습니다.

### Priority 4 - Material Classification / Binning

단일 fullscreen uber resolve 구조를 넘어가야 합니다.

가능한 방향:

- `pipelineKey` 기반 visible pixel 분류
- material/feature class별 screen-space 리스트 구성
- material bucket별 더 좁은 resolve shader 실행 (per-material indirect dispatch)

버킷 수를 소수로만 나눠도 현재 올인원 구조보다 개선 여지가 큽니다.

부수 효과: bucket별 dispatch 안에서는 material/geometry descriptor 인덱스가 **uniform**이 되므로 root 상수로 바인딩할 수 있고, 현재 fullscreen resolve가 강제로 쓰는 `NonUniformResourceIndex`(및 그 wave scalarization 비용)를 제거할 수 있습니다. (`ClusterDAG-AMD-NonUniform-Bindless-Bug.md` 참고)

### Priority 5 - Resolve 출력 재검토

경로가 legacy `GBuffer` 전체를 계속 재구성할지, 혹은 일부 출력을 축소/재패킹하거나 필요할 때만 생성할지 결정해야 합니다.

이는 deferred 호환성을 얼마나 강하게 유지할지와 `ClusterDAG` 경로 추가 최적화 사이의 균형에 따라 달라집니다.

### Priority 6 - 디버깅 및 Validation 강화

다음 항목에 대한 명시적 디버그/검증 지원을 추가합니다.

- visibility entry 유효성
- page-local fetch 실패
- primitive-id overflow
- resolve material mismatch
- visibility/debug view 시각화

경로가 계속 진화하는 동안 디버깅 효율을 크게 높여줍니다.

### Priority 7 - 잔여 Fallback 의존성 점검

visibility 경로를 `ClusterDAG` 기본 렌더 경로로 간주하게 되면, legacy 전역 packed geometry에 대한 숨은 의존성을 계속 줄여나갈 필요가 있습니다.
