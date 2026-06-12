# SparseSdfGI Internal Half-Resolution v1

## 요약

- FFX BrixelizerGI처럼 SparseSdfGI 내부 GI 산출만 1/2 해상도로 실행하고, bilateral upsample로 full-res `DiffuseGI`를 복원한다.
- 기본값 Off. `SparseSdfGIInternalHalfResolution` 토글로 A/B 비교한다. (구 ini 키 `SparseSdfGITraceHalfResolution`는 별칭으로 호환.)
- 외부 계약은 불변이다: `DiffuseGI`/denoiser input SH/variance는 항상 full-res이고 DeferredLighting/denoiser는 변경 없음.
- Debug 모드(`DebugMode != Off`, `ProbeDebugMode != Off`)는 full-res를 유지해 시각화가 흐려지지 않는다.

## 상수

- `OutputWidth`/`OutputHeight`가 dispatch/쓰기 해상도(half 또는 full)다.
- `FullOutputWidth`/`FullOutputHeight`가 full-res 해상도다.
- `IsInternalHalfResolution()`은 둘이 다를 때 참이다.
- half 차원은 `(full + 1) / 2`(C++ `GetInternalHalfDimension`, HLSL `halfMaxPixel`가 동일 식).

## Trace / Probe Interpolate

- half일 때 `CSDiffuseTrace`/`CSInterpolateScreenProbes`는 half 차원으로 dispatch하고 half-res transient(diffuse/SH/variance)에 쓴다.
- depth/GBuffer 로드는 대표 full-res 픽셀로 변환한다: `fullPixel = min(lowPixel * 2 + 1, fullSize - 1)`.
- ray reconstruction(UV/world position)은 full-res 차원 기준(`ReconstructWorldPositionFull`, `GetFullScreenUv`)으로 계산한다.
- probe density(`ProbeCountX/Y`)는 full-res 기준 유지. v1은 probe 밀도를 줄이지 않고 interpolate 출력 해상도만 줄인다.

## Upsample

- 새 entry `CSUpsampleSparseSdfGI`. 입력: half diffuse/SH/variance + full depth/GBufferA. 출력: full `DiffuseGI` + full input SH + full variance.
- bilateral 2x2: full 픽셀의 대응 half 좌표 주변 4탭, weight = bilinear × 상대 깊이(`exp(-|d-d'|/max(d*0.1, 1e-3))`) × 법선(`pow(dot, 32)`).
- 탭의 깊이/법선은 그 half 픽셀의 대표 full 픽셀(`half*2+1`)에서 읽어 trace와 동일한 매핑을 쓴다.
- weight sum이 무너지면(깊이/법선 불연속) nearest half 샘플로 폴백하고 **variance=1**로 표시한다. 디노이저가 그 픽셀을 강하게 블러하도록 둔다.

## 패스 순서

```text
direct: Trace(half) -> Upsample -> IrradianceCacheUpdate
probe:  Probe Spawn/Trace/Interpolate(half) -> Upsample -> IrradianceCacheUpdate
```

- `AddIrradianceCacheUpdatePasses`는 full-res `DiffuseGI`를 읽으므로 반드시 Upsample 이후에 실행한다.
- full-res 경로에서는 `InternalDiffuseGIHandle`이 비어 Upsample이 early-out하고, Trace/Interpolate가 `DiffuseGI`에 직접 쓴다.

## 성능 기대치

- direct trace 경로: `CSDiffuseTrace`(1spp full-res)가 1/4로 줄어 확실한 이득. upsample 1패스가 추가된다.
- probe 경로: probe trace 비용은 per-probe라 불변이고 interpolate만 1/4이 되는 대신 upsample이 추가되므로 이득이 작거나 중립일 수 있다. "probe 경로는 안 빨라졌다"는 실패가 아니라 예상 결과다.

## 검증

- 토글 on/off 비교: full-res 영역 동등, half-res에서 GPU 타이밍 하락(특히 direct 경로).
- 홀수 뷰포트(예: 1279x719)에서 검은 체커/쓰기 갭 없음.
- 깊이/법선 경계, 얇은 지오메트리에서 누설/링잉 없음(폴백 variance=1이 디노이저로 흡수).
- denoiser on/off, multi-bounce on/off 조합 정상.
- 디노이저 히스토리는 기존 resize/config 경로로만 리셋되고 매 프레임 리셋되지 않음.

## 향후 과제

- 대표 픽셀 고정 코너(`+1,+1`)는 full 픽셀의 3/4 지오메트리를 GI가 영구히 안 본다. 얇은 지오메트리는 매 프레임 bilateral 폴백에 의존. 프레임별 코너 jitter(quincunx)가 v2 후보.
- probe 밀도까지 줄이는 모드는 v1 범위 밖.
