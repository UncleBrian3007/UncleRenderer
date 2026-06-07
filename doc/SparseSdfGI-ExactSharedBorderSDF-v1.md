# SparseSdfGI Exact Shared-Border SDF v1

## Summary

SparseSdfGI의 `Gradient` 디버그에서 보이는 brick 단위 큐브/계단 artifact는 노멀 시각화 문제가 아니라 SDF field 자체가 brick 경계에서 불연속적인 것이 핵심 원인이다.

현재 UncleRenderer는 brick마다 독립적으로 reference triangle을 모은 뒤, narrow seed를 만들고 brick-local Eikonal relaxation으로 내부 거리를 채운다. 이 방식은 빠르게 prototype하기 좋지만, brick 밖을 `1.0` far로 취급하므로 경계 근처 거리와 gradient가 이웃 brick과 맞지 않는다.

FFX Brixelizer는 8개 SDF sample을 8칸이 아니라 7개 interval로 해석한다. 즉 `8 samples = 7 intervals` 구조라 인접 brick이 경계 sample 위치를 공유한다. 또한 표면 근처는 triangle까지의 직접 거리 계산으로 채워져 Gradient가 더 부드럽다.

이 문서는 SparseSdfGI의 SDF 빌드/샘플링을 FFX에 가까운 **exact triangle-distance + shared-border brick layout**으로 전환하기 위한 계획이다.

## Current Problem

- 현재 brick layout은 `BrickVoxelResolution = 8`, `AtlasResolution = 64 * 8 = 512`이다.
- world position to atlas 변환은 `VoxelSize` 기준의 512 grid로 직접 매핑된다.
- brick-local solve는 `localCoord 0..7`만 보고, `EikonalLoad`는 brick 밖 neighbor를 `1.0`으로 처리한다.
- manual trilinear sampling은 brick-map indirection으로 이웃 brick voxel을 읽을 수 있지만, 저장된 SDF 값 자체가 경계에서 서로 맞지 않는다.
- 따라서 `ComputeSdfNormal`의 중앙차분을 아무리 부드럽게 해도 Gradient 모드에서는 brick seam과 cube pattern이 남는다.

## FFX Reference Notes

- Brixelizer는 brick당 8개 sample을 저장하지만 sample 간격은 `voxel_size / 7`로 잡는다.
- trace 시 brick UVW 범위도 `uvw_min + 7 / atlasSize` 형태로 잡아 brick 내부 8 sample이 7 interval을 덮게 한다.
- `EmitSDF`는 reference triangle에 대해 sample별 triangle distance를 직접 계산하고 atomic min으로 SDF atlas를 채운다.
- Brixelizer에는 dirty-brick Eikonal/merge 계열 코드도 있으므로 "Eikonal이 전혀 없다"라고 단정하지 않는다. 다만 Gradient 품질 차이의 핵심은 shared-border sample layout과 표면 근처 exact distance field다.
- FidelityFX SDK 코드, HLSL, 상수 테이블은 복사하지 않고 구조와 좌표계 아이디어만 참고한다.

## V4.0 Plan: Exact Shared-Border Mode

### Build Mode

- 새 build mode를 추가한다.
  - `LegacyEikonal`: 기존 brick-local Eikonal 경로.
  - `ExactSharedBorder`: exact triangle-distance + 8-for-7 shared-border 경로.
- 기본값은 `LegacyEikonal`로 유지한다.
- config/UI 후보:
  - `SparseSdfGISdfBuildMode = 0/1`
  - UI label: `SDF Build Mode: Legacy Eikonal / Exact Shared Border`
- build mode는 SDF cache signature에 포함한다.
- mode 변경 시 SDF cache, brick radiance, brick irradiance, probe history를 invalidate한다.

### Coordinate Layout

- exact mode에서는 brick 하나가 world-space에서 `7 * BrixelSize` interval을 덮는다.
- `BrixelSize`는 현재 `VoxelSize`와 분리해 명명한다.
  - `BrixelSize = VoxelSize`로 두면 exact mode의 cascade coverage가 legacy 대비 `7/8`로 줄어든다.
  - A/B 비교 기본값은 `BrixelSize = VoxelSize * 8 / 7`로 두어 legacy coverage와 맞춘다.
  - UI/debug에는 legacy/eXact의 실제 cascade extent, voxel size, brixel size를 같이 표시한다.
  - 이렇게 하면 dense cascade logical sample domain은 `64 * 7 + 1 = 449` sample positions가 된다.
- local sample `0..7`의 world position:

```text
sampleCoord = brickCoord * 7 + localCoord
worldPos = CascadeMin + sampleCoord * BrixelSize
```

- atlas storage는 기존 `64 * 8 = 512` physical texture를 유지한다.
- physical atlas address는 계속:

```text
atlasCoord = brickCoord * 8 + localCoord
```

- logical coordinate와 physical atlas coordinate를 분리해 이름으로 구분한다.
  - `logicalSampleCoord`: shared-border 7-interval domain.
  - `atlasCoord`: physical 512 atlas storage coordinate.

### Exact Solve

- `CSSolveBrickReferences`에 exact variant를 추가한다.
- 기존 triangle pool, brick references, occupied brick indirect dispatch는 유지한다.
- 각 local sample은 자기 brick reference list의 triangle 전체를 순회해 `PointTriangleDistance`의 min을 구한다.
- exact mode에서는 Eikonal relaxation을 생략한다.
- SDF encoding은 첫 구현에서 기존 linear normalized format을 유지한다.
  - `encoded = saturate(distance / (7 * BrixelSize))`
  - `decoded = encoded * (7 * BrixelSize)`
- decode helper는 build mode별로 분기한다.
  - legacy: `decoded = encoded * (8 * VoxelSize)`
  - exact: `decoded = encoded * (7 * BrixelSize)`
- FFX식 signed/sqrt compression은 후속 단계로 둔다.
- empty/invalid brick은 init pass가 쓴 `SdfAtlas = 1.0`, `BrickMetadata = 0`을 유지한다.

### Reference Emission

- 기존 V3.1 split reference binning을 유지한다.
- brick overlap 계산은 surface band를 포함해 확장한다.
- exact mode에서는 brick world bounds가 `brickCoord * 7 * BrixelSize` 기준이므로 reference binning도 이 layout을 사용한다.
- 경계 sample 공유를 위해 triangle이 brick 경계에 걸치면 양쪽 brick reference list에 들어가야 한다.
- seam-free 품질은 `brick N local 7`과 `brick N+1 local 0`이 같은 world position에서 같은 영향 triangle set을 보도록 보장하는 데 달려 있다.
- brick reference AABB inflation은 exact brick bounds 기준으로 최소 `surfaceBand` 이상 확장한다.
- trace correctness 관점에서는 reference가 없는 더 가까운 surface 때문에 큰 distance가 저장되면 sphere tracing overstep이 생길 수 있으므로, RayTrace/StepCount 검증으로 누락과 leak을 확인한다.

### Sampling / Trace

- exact mode 전용 `SampleSdfAtlasExactSharedBorder` 계열 helper를 추가한다.
- world position을 logical sample coordinate로 변환한다.

```text
logical = (worldPos - CascadeMin) / BrixelSize
sampleCoord = logical
baseLogical = floor(sampleCoord)
frac = sampleCoord - baseLogical
```

- `baseLogical`을 physical atlas coordinate로 변환한다.

```text
brickCoord = baseLogical / 7
localCoord = baseLogical - brickCoord * 7
baseLocal = baseLogical - brickCoord * 7   // 0..6 for a trilinear cell
tap local samples = baseLocal and baseLocal + 1   // always 0..7 inside the same physical brick
```

- exact shared-border trilinear cell은 `baseLogical % 7`이 `0..6`인 셀 기준으로 같은 brick의 local `0..7` 안에 닫힌다. 따라서 일반 trilinear 8-tap은 neighbor brick remap이 필요 없다.
- 경계 정수 위치는 canonical owner를 정해 샘플링 일관성을 유지한다. 예를 들어 logical coordinate가 정확히 `7k`이면 다음 brick의 local `0` 또는 이전 brick의 local `7` 중 하나로 일관되게 정규화한다.
- invalid/out-of-cascade sample은 `1.0` empty를 반환한다.
- `ComputeSdfNormal`, `TraceSdf`, `TraceSdfDebugSurface`, `DebugGradient`, `CSDiffuseTrace`, screen probe trace는 build mode에 맞는 sample helper를 사용한다.

### Metadata / AABB Skip

- occupied threshold는 기존 trace surface threshold와 동일하게 유지한다.
- metadata local AABB는 `0..7` local sample bounds로 pack한다.
- exact mode에서 trace AABB world bounds는 `brickCoord * 7 * BrixelSize + localAabb` 기준으로 계산한다.
- manual trilinear/shared-border bleed를 덮기 위해 margin은 `1.0 * BrixelSize`로 시작한다.

## Future Work

### V4.1: Sqrt / Signed Encoding

- FFX처럼 distance를 sqrt-compressed format으로 저장하는 옵션을 검토한다.
- unsigned-only exact mode가 안정화된 뒤 signed distance를 추가한다.
- trace step decode와 hit threshold를 encoding별로 분리한다.

### V4.2: Exact Mode Performance

- 우선순위는 낮다. exact mode는 기존 per-voxel reference distance loop를 유지하면서 Eikonal relaxation을 제거하므로, solve cost가 legacy와 비슷하거나 더 낮을 수 있다.
- Force Rebuild에서 exact solve cost를 측정한다.
- reference list traversal이 병목이면:
  - LDS triangle cache 크기 조정.
  - linked-list reference를 scan/compact contiguous layout으로 전환.
  - large triangle reference culling 정밀화.
- 성능 최적화는 shared sample consistency와 trace correctness 검증 이후 측정 기반으로 진행한다.

### V4.3: Shared-Border Default Candidate

- Duck/Sponza/large-plane scene에서 Gradient, RayTrace, StepCount, diffuse GI를 비교한다.
- seam 감소가 확실하고 rebuild cost가 허용 가능하면 default build mode 전환을 검토한다.
- Legacy Eikonal은 debug/compat 경로로 남긴다.

### V4.4: Brick SH / Radiance Cache Alignment

- exact shared-border SDF가 안정화되면 brick irradiance/radiance cache의 위치 해석도 logical 7-interval layout과 맞춘다.
- P1 `BrickIrradiance`와 P2 screen probe SH는 처음에는 기존 brick index 기준을 유지하되, final brick SH resolve 단계에서 coordinate convention을 통일한다.

## Longer-Term Roadmap

### V4.5: Debug Visualization Parity

- FFX Brixelizer debug modes 중 `Distance`, `UVW`, `Iterations`, `Brick ID`, `Cascade ID`를 SparseSdfGI debug mode로 단계적으로 추가한다.
- exact mode와 legacy mode를 같은 카메라/scene에서 비교할 수 있도록 debug output을 동일한 trace payload 기준으로 정리한다.
- Gradient 모드에는 SDF build mode와 sample layout을 UI/debug text로 표시해 혼동을 줄인다.

### V5.0: Sparse / Clipmap Transition

- dense single-cascade 검증이 끝나면 sparse brick allocator와 clipmap wrapping을 다시 도입한다.
- shared-border layout에서는 brick 경계 sample이 이웃 brick과 연결되므로, invalid neighbor 처리와 allocation policy를 먼저 정의한다.
- static full rebuild에서 dirty-brick incremental update로 확장할 때도 exact shared-border coordinate convention을 유지한다.

### V5.1: Dirty Brick Exact Rebuild

- Brixelizer의 dirty-brick dispatch 구조처럼, 변한 brick과 경계 neighbor만 exact solve 대상으로 모은다.
- shared-border sample을 쓰므로 dirty brick 주변 1-ring neighbor도 재검토 대상에 포함한다.
- static cache-hit frame에서는 기존처럼 build pass를 생략하고, dynamic/change frame에서는 dirty solve만 실행한다.

### V6.0: Directional Brick Radiance / SH Resolve

- SDF layout이 안정화된 뒤 P1 `BrickIrradiance`를 DC-only RGB에서 L1 또는 2nd-order SH로 확장한다.
- screen probe는 cache producer가 되고, 최종 diffuse GI는 brick SH를 surface normal로 평가하는 BrixelizerGI에 가까운 구조를 검토한다.
- 이 단계에서 `SparseSdfGI-BrixelizerGI-Roadmap-v1.md`의 P2/P3/P4와 통합해 screen probe SH, brick SH propagation, separable blur를 한 흐름으로 정리한다.

### V6.1: Encoding / Memory Optimization

- exact SDF와 brick SH가 모두 들어간 뒤 메모리 사용량을 재평가한다.
- SDF는 sqrt/signed compression, SH는 half-packed coefficient buffer, references는 scan/compact layout을 후보로 둔다.
- 목표는 FFX와 같은 구조를 그대로 복사하는 것이 아니라, UncleRenderer의 RenderGraph/bindless 구조에 맞는 native representation을 확정하는 것이다.

## Test Plan

- DXC compile:
  - `CSInitReferenceBuild`
  - `CSEmitTriangleReferences`
  - legacy `CSSolveBrickReferences`
  - exact shared-border `CSSolveBrickReferences`
  - `CSDebugTrace`
  - `CSDiffuseTrace`
- Debug x64 MSBuild 통과.
- Runtime validation:
  - Duck/Sponza에서 `LegacyEikonal`과 `ExactSharedBorder`를 Gradient mode로 A/B 비교한다.
  - legacy와 exact의 cascade coverage가 같은지 UI/debug의 extent, voxel size, brixel size로 확인한다.
  - `Shared Sample Mismatch` debug로 인접 brick의 `local7 - neighbor local0` absolute difference가 낮은지 확인한다.
  - exact mode에서 brick cube seam이 줄고, 곡면/벽-바닥 접합부 normal이 더 연속적인지 확인한다.
  - RayTrace/StepCount에서 overstep, hit 누락, leak이 없는지 확인한다.
  - DebugMode 0 diffuse output이 non-black이고 큰 구멍이 없는지 확인한다.
  - Force Rebuild 후 triangle/reference overflow가 0인지 확인한다.
  - cache-hit frame에서 기존 static cache가 유지되는지 확인한다.
- Performance validation:
  - Force Rebuild의 solve GPU time을 legacy와 exact로 비교한다.
  - exact solve가 legacy보다 크게 느리면 V4.2 최적화를 먼저 진행한다.

## Assumptions

- 첫 목표는 Gradient/SDF 품질 검증이며 exact mode를 즉시 기본값으로 바꾸지 않는다.
- dense single-cascade, current triangle pool/reference binning, occupied-brick indirect dispatch는 유지한다.
- sparse allocator, multi-cascade clipmap, full FFX trace payload, signed SDF는 이번 단계 범위 밖이다.
- 현재 워크트리에 있는 다른 수정 파일은 건드리지 않고, 관련 변경만 별도 commit한다.
