# SparseSdfGI Radiance Denoise v1

SparseSdfGI radiance denoise 작업은 SDF 빌드 백본(`SparseSdfGI-v3.md`)과 radiance cache 계획(`SparseSdfGI-Radiance-v1.md`)에서 분리해 다룬다. AMD FidelityFX BrixelizerGI의 denoise / screen-probe 구조에서 영감을 받았지만, FidelityFX SDK 소스, 셰이더 코드, 상수, 데이터 테이블은 복사하지 않는다.

## 현재 상태

- **Phase 0 + Phase 1은 공용 `FDiffuseGIDenoiser`로 구현 완료**됐다.
- denoiser는 SparseSdfGI 전용이 아니다. ReSTIR GI와 SparseSdfGI가 활성 diffuse GI source에 따라 같은 SH-irradiance SVGF 스타일 backend에 입력을 넣는다.
- SparseSdfGI는 full-resolution `DiffuseGIInputSH`와 `DiffuseGIVariance`를 출력한다. `FDiffuseGIDenoiser`는 pre-blur, temporal accumulation, SH/depth mip 생성, history reconstruction, final bilateral blur를 처리한다.
- **Road B v1은 선택 가능한 screen-probe front-end로 구현 완료**됐다. 이 경로는 denoiser와 composite 계약을 유지하면서 per-pixel 1-ray 입력 생성을 대체한다.

## Phase 0/1 Backend

공용 backend pass chain은 다음과 같다.

```text
CSPreBlur
  -> CSTemporalAccumulation
  -> ShMipGen / LinearDepthMipGen
  -> Denoiser HistoryBlur
  -> CSFinalBlur
```

backend는 frame overlap에 안전한 history buffer를 유지한다. 대상은 SH, irradiance, history count, 이전 linear depth, 이전 normal이다. front-end가 ReSTIR GI이든, SparseSdfGI per-pixel trace이든, SparseSdfGI screen probe이든 backend는 full-resolution SH와 variance 입력을 기대한다.

## Road B: Screen Probe + SH Front-End

Road B는 SparseSdfGI의 per-pixel 1-ray front-end를 희소 screen probe로 교체한다.

```text
CSSpawnScreenProbes
  -> CSTraceScreenProbes
  -> CSInterpolateScreenProbes
  -> shared FDiffuseGIDenoiser
```

기본값은 다음과 같다.

- `bSparseSdfGIUseScreenProbes = false`
- `SparseSdfGIProbeTileSize = 8`
- `SparseSdfGIProbeRaysPerProbe = 16`
- `SparseSdfGIProbeDebugMode = 0`
- `bSparseSdfGIProbeTemporalReuse = false`

probe path는 SparseSdfGI debug mode가 `Off`일 때만 활성화된다. 기존 SDF debug view들은 기존 trace/debug path를 그대로 사용한다.

### Probe Spawn

각 screen tile은 probe 하나를 만든다. probe는 representative pixel, world position, normal, depth, validity를 저장한다. 대표 픽셀은 기본적으로 tile 중앙을 **프레임 간 고정**으로 쓰되, 중앙 depth가 invalid이면 tile 내부에서 짧게 valid depth를 찾는다.

`bSparseSdfGIProbeSpawnJitter`(ImGui "Probe Spawn Jitter")를 켜면 대표 픽셀을 프레임마다 tile 내부로 jitter해 coverage를 보완한다. jitter는 probe 앵커를 매 프레임 흔들기 때문에 **probe temporal과 함께 쓸 때만** 의미가 있다(temporal이 jitter된 샘플을 tile 단위로 누적). temporal 없이 jitter만 켜면 interpolation 패턴이 매 프레임 이동해 **떨리는 얼룩**으로 보이므로 기본값은 off다.

### Probe Trace

각 probe는 기존 SDF / brick-radiance path를 통해 여러 개의 cosine-hemisphere ray를 trace한다. probe trace는 `CSDiffuseTrace`와 같은 hit shading helper를 공유한다.

ray 방향(및 spawn jitter)은 ReSTIR GI와 **동일한 blue-noise Sobol 샘플러**(`BlueNoiseSobolSampler.hlsli`)를 쓴다. 기존 hash white-noise(`Random2`) 대신 저불일치 stratified 샘플을 써서 같은 ray 수에서 분산이 낮아지고 temporal 수렴이 빨라진다. 샘플러는 텍스처 인덱스를 파라미터로 받도록 통일했고(ReSTIR도 동일 호출), blue-noise 텍스처는 `FDeferredRenderer`가 소유한 것을 공유한다. probe당 ray `i`는 `SampleIndex=i`, 프레임 간에는 sampler가 pixel base를 골든비율로 shift해 decorrelate한다.

screen probe를 끈 **per-pixel `CSDiffuseTrace`** front-end도 동일 blue-noise Sobol 샘플러를 쓴다(픽셀당 1 ray). SparseSdfGI의 모든 GI 샘플링이 `Random2` 없이 ReSTIR와 같은 샘플러로 통일됐다. 두 trace 경로 모두 root signature가 64-DWORD 한계라, blue-noise 인덱스 2개와 한 쌍의 기존 인덱스를 각각 `16:16` packed DWORD로 묶어 bindless 상수에 싣는다.

- miss: sky radiance
- hit: valid한 brick radiance cache 우선 사용
- cache miss: direct-light bounce fallback

probe는 cosine-hemisphere ray들의 radiance를 평균낸 뒤, per-pixel path와 동일하게 **DC-only irradiance SH**(`ProjectIrradianceSh`)로 emit한다. per-pixel path가 단일 ray radiance를 그대로 irradiance로 내보내는 것과 같은 추정량(cosine-weighted radiance 평균)이라, probe path를 켜고 꺼도 **밝기가 일치**한다(toggle 시 밝기 점프 없음). variance는 probe ray luminance variance를 누적해 산출한다.

> **밝기 규약 결정 (a)**: directional L1 SH 대신 DC-only irradiance를 emit해 per-pixel 캘리브레이션과의 parity를 우선했다. directional 경로 (b)는 아래 Follow-Up Work 참고.

### Probe Interpolation

full-resolution pixel은 3x3 probe neighborhood에서 값을 모은다. interpolation weight는 screen distance, normal agreement, depth difference, plane distance를 사용한다. plane distance는 discontinuity 근처 light leak을 줄이기 위한 필수 항목이다.

출력 계약은 바뀌지 않는다.

- `DiffuseGI`: full-resolution irradiance preview / output
- `DiffuseGIInputSH`: `FDiffuseGIDenoiser`가 읽는 full-resolution SH
- `DiffuseGIVariance`: `FDiffuseGIDenoiser`가 읽는 full-resolution variance

기여 가능한 valid probe가 하나도 없으면 pixel은 zero SH와 high variance를 쓴다. 그러면 denoiser의 history reconstruction이 보수적으로 빈 구간을 메운다.

### Probe Temporal Reuse

`bSparseSdfGIProbeTemporalReuse`가 켜지면 `CSTraceScreenProbes`가 probe SH를 프레임 간 누적한다. probe당 ray 수가 적어(16~32) 매 프레임 probe SH가 흔들리는데, 이를 probe 단계에서 평균내 떨림을 근본적으로 줄인다.

- **persistent ring buffer**: `ProbeHistory`(`FScreenProbeHistory` = world pos + sample count + normal/depth + packed SH)를 frame-overlap-safe ring으로 둔다(brick radiance cache와 동일한 read=N-1 / write=N 슬롯 + fence 기반 `ProbeHistoryValid` 커밋).
- **motion-vector reprojection** (`bSparseSdfGIProbeMotionReproject`, ImGui "Probe Motion Reproject", 기본 on): spawn 패스가 대표 픽셀의 velocity를 샘플해 이전 프레임 screen tile을 찾고, 그 prev-probe-index를 probe header에 실어(trace b2가 만석이라 trace에 새 인덱스를 추가하지 않는다) trace가 해당 history와 블렌드한다. 카메라가 움직여도 temporal 누적이 유지된다. 끄거나 velocity≈0이면 same-tile로 동작. 화면 밖으로 reproject되면 history 무효(reset).
- **geometry reject**: 어느 경로든 history의 world pos/normal/depth를 현재 probe와 비교해(`dot(normal) > 0.9`, depth 5%, plane 거리 ≤ `VoxelSize*4`) 불일치(=disocclusion/잘못된 reproject)면 reject하고 current로 리셋한다.
- **누적**: `sampleCount = min(prevCount+1, 32)`, `alpha = max(1/sampleCount, 0.05)`로 `LerpSh(history, current, alpha)`. DC-only SH라 누적 후에도 per-pixel parity가 유지된다. variance는 `normalizedVariance / sampleCount`로 낮춰 denoiser가 수렴된 입력을 덜 blur하게 한다.
- **무효화**: resize / tile size 변경 / screen-probe·temporal 토글 / SDF rebuild 시 history valid 플래그를 클리어한다.

> **주의(이중 temporal)**: probe temporal과 full-res `FDiffuseGIDenoiser` temporal이 동시에 걸리면 lag가 곱해질 수 있다. probe temporal이 수렴을 책임지므로, 필요하면 denoiser temporal 강도를 source-aware로 낮추는 후속 작업이 있다(Follow-Up).

## Variance Policy

per-pixel 1-ray 입력은 white noise가 지배적이므로 강한 variance floor가 필요하다. 반면 probe 입력의 주요 artifact는 interpolation confidence, blocking, edge leak이다. 따라서 probe variance는 다음 식으로 시작한다.

```text
saturate(probe ray luminance variance + (1 - interpolation confidence))
```

작은 floor 값은 `0.05`로 둔다.

## Debug Views

`SparseSdfGIProbeDebugMode`는 SDF debug mode와 독립적이다.

- `0`: Off
- `1`: Probe placement
- `2`: Validity / interpolation confidence
- `3`: Probe hit ratio
- `4`: Probe variance
- `5`: Interpolation confidence heat
- `6`: Probe irradiance

`ProbeDebugMode != 0`이면 interpolation pass가 denoiser 입력(SH / variance)을 publish하지 않는다. 그러면 denoiser가 입력 없이 스킵되고 composite가 raw `DiffuseGI`(probe debug 색)로 폴백한다 — SDF debug mode와 동일한 메커니즘. 따라서 probe debug view는 denoiser on/off와 무관하게 보인다.

## Follow-Up Work

- probe temporal이 켜졌을 때 double-temporal ghosting을 피하기 위한 source-aware denoiser temporal strength 조정.
- geometry discontinuity를 위한 adaptive probe placement.
- 비용 절감을 위한 separable final blur 변형.
- screen-probe SH만으로 부족할 때에 한해 optional directional brick radiance cache 검토.
- **(b) directional probe SH**: 현재는 DC-only irradiance로 parity를 맞췄다. probe ray별 radiance를 `ProjectSh`로 L1 SH에 투영하고 interpolation 후 `UnprojectIrradiance`로 diffuse cosine convolution을 적용하면, probe별 방향성 indirect를 얻을 수 있다. 단 이 경로는 per-pixel DC-only 대비 절대 밝기가 달라지므로(상수 radiance 기준 약 1.4–1.5배), 채택 시 per-pixel/Intensity를 재캘리브레이션해 toggle 밝기 parity를 다시 맞춰야 한다. directional은 cosine-sampled 방향과 `ProjectSh`의 2π(uniform-sphere) 투영 상수가 섞여 절대 스케일이 ad-hoc인 점도 함께 정리 필요.

## 튜닝 / 참고 메모

- **`normalWeight = pow(dot, 32)`가 매우 날카로움**: 곡면/엣지에서 probe가 대거 reject → `totalWeight` 붕괴 → 고 variance로 denoiser에 과의존 → 곡면이 얼룩질 수 있음. `pow 8~16`으로 완화해 비교 권장.
- **`ProbeVariance`를 float4로 잡고 `.x`/`.y`만 사용**(probe당 8바이트 낭비) — 무해.
- **interpolation `screenWeight`가 jitter된 실제 probe 픽셀이 아니라 기하학적 tile 중심을 씀**(world-space weight는 실제 픽셀 사용해서 OK) — 사소.

