# SparseSdfGI Brick SH 전파 v1

## 요약

- Brick radiance/irradiance 캐시를 `float4(rgb, confidence)` 대신 L1 SH로 저장한다.
- SH 레이아웃은 `CommonSH.hlsli`의 `FPackedSh`를 사용한다: luminance L1 SH + DC chroma.
- 전파는 FFX BrixelizerGI 스타일의 26-이웃 smoothing 패스로, 할당된 logical brick을 대상으로 한다.
- FFX와 동일하게 **valid base brick만 갱신**한다. invalid brick은 그대로 통과시킨다. 주입이 screen-space라 off-screen 표면의 brick은 count 0이고, 그 brick들의 광원은 trace의 directBounce 폴백이다. invalid brick을 이웃으로 채워 valid로 만들면 그늘진 이웃의 어두운 값이 directBounce 폴백을 가려 간접광이 붕괴한다.
- 따라서 v1의 전파는 hole-fill이 아니라 valid brick 간 smoothing/안정화다. hole-fill은 ray-hit 주입(FFX 방식)이 도입되어 cache가 1차 광원이 된 후에만 다시 검토한다.
- `SparseSdfGIPropagateBrickSH`(기본값 true)로 전파를 토글한다. 끄면 resolve가 persistent 슬롯에 직접 쓰고 전파 패스를 건너뛰므로, SH 캐시 포맷은 동일하게 유지되어 A/B 비교가 가능하다.

## 캐시 시맨틱

- `FBrickShGpu`는 32바이트다: packed SH `uint4`, `SampleCount`, 패딩.
- `FBrickShAccumGpu`는 32바이트이며 resolve 전에 signed SH 계수를 원자적으로 누적한다.
- 양자화 클램프는 `SPARSE_SDF_GI_BRICK_SH_MAX_COEFF = 4 * RADIANCE_MAX_SAMPLE`이다. `ProjectSh`가 L1 계수를 radiance 클램프보다 최대 `2*pi*L1 (~3.07배)` 크게 스케일하기 때문에, radiance 도메인으로 클램프하면 luminance만 잘리고 chroma는 그대로 남아 색상과 dominant direction이 왜곡된다.
- `SPARSE_SDF_GI_RADIANCE_HISTORY_DECAY`는 `0.985`를 유지한다.
- Confidence는 `saturate(SampleCount / 64)`다.
- 기존 `SPARSE_SDF_GI_RADIANCE_CONFIDENCE_THRESHOLD = 0.05`는 최소 유효 샘플 수 4에 해당한다.

## 읽기 측 평가

- GI 레이 바운스는 SDF gradient가 신뢰 가능할 때 hit normal로 brick radiance를 평가한다.
- SDF gradient가 약해 normal이 `-traceDirection` 폴백이면, 방향성 `UnprojectIrradiance` 대신 `ApproxRadiance`로 평가한다.
- Multi-bounce inject는 GBuffer normal로 brick irradiance를 읽고, normal이 무효일 때만 DC로 폴백한다.

## 전파

- Dispatch는 `CascadeCount * 64^3` logical brick을 커버한다.
- Base logical brick이 할당되어 있어야 하며, 아니면 스레드가 즉시 return한다.
- Base sample이 invalid면 그대로 dest에 통과시키고 끝낸다. 전파로 validity가 새로 생기지 않는다.
- Base weight는 `SampleCount^2`다.
- 유효한 이웃은 같은 캐스케이드의 3x3x3 logical brick 이웃(중심 제외)에서 샘플한다.
- 이웃 weight는 `1 / distanceSq`다.
- 이웃이 기여하는 sample count에는 `SPARSE_SDF_GI_BRICK_SH_PROPAGATION_DECAY = 0.9`를 곱한다. 빌린 confidence가 hop마다 감쇠하므로, brick 단위 빛 전진이 무한히 이어지지 않고 유효성 임계 기준 약 30 hop에서 소멸한다.
- Direction gating은 정규화된 방향성 `length(ShY.yzw) / max(abs(ShY.x), eps)`를 사용한다.
- Gating은 base와 이웃 양쪽이 충분히 방향성이 있을 때만 적용한다.

## 노트

- 메모리는 SH 캐시 버퍼당 약 8 MiB다 (`262144 * 32B`).
- Radiance 히스토리 슬롯, irradiance 슬롯, accum 버퍼를 합치면 기존 `float4` 캐시 대비 약 2배다.
- 실효 전파 비용은 dispatch 상한이 아니라 할당 brick 수 × 26탭에 가깝다. 미할당 base brick이 early out하기 때문이다.
- 향후 half-resolution GI는 `Upsample -> Irradiance Accumulate/Resolve/Propagate` 순서를 유지해야 한다.
- 전파는 persistent write 슬롯의 미할당 물리 엔트리를 절대 쓰지 않으므로 stale 엔트리가 남는다. 읽기가 항상 cascade brick map을 경유하고 리빌드가 radiance/irradiance 히스토리를 무효화하기 때문에만 안전하다. free-list 도입으로 물리 id의 소유자가 리빌드 없이 바뀌게 되면 재검토해야 한다.
