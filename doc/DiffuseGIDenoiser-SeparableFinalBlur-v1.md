# Diffuse GI 디노이저 Separable Final Blur v1

## 요약

- FFX `FfxBrixelizerGIBlurGI`를 참고해 디노이저의 final blur를 sample count 기반 가변 반경 separable(X/Y) 패스로 교체한다.
- 기존 `CSFinalBlur`(4-tap surface-projected Poisson, 매 픽셀 항상 실행)는 유지되며, `DiffuseGIDenoiserSeparableBlur`(기본값 true) 토글로 두 경로를 A/B 비교한다.
- 수렴 픽셀(`HistoryCount`가 차오른 픽셀)은 반경이 0으로 떨어져 탭 루프 없이 통과한다. steady-state 비용이 탭 비용에서 복사 비용으로 줄어든다.

## 커널

`CSSeparableFinalBlur` 하나가 X/Y 두 방향을 처리하며, 방향은 `BlurDirectionY` 상수로 선택한다.

- 반경: `Radius = 8 * smoothstep(1 - HistoryCount / 32)` 픽셀. 수렴(count 32)에서 0, 신규 픽셀에서 8.
- `Radius < 0.5`면 소스 SH를 그대로 통과시킨다 (early-out).
- 탭: 방향당 `{-2, -1, +1, +2} * (Radius / 2)` 4탭 + 중심. 두 패스 합산 커버리지는 `(2 * Radius + 1)^2` 분리 커널로, 기존 4-tap Poisson보다 넓다.
- 가중치: 가우시안(`exp(-0.5 * i^2)`) × 상대 깊이(`exp(-|d - d'| / max(d * 0.1, 1e-3))`) × 법선(`pow(dot, 32)`). 히스토리 재구성 수정 때 정리한 상대 깊이 시그마 컨벤션을 따른다.
- 마지막(Y) 패스 여부는 `OutHistoryIrradianceIndex` 유효성으로 판별하고, Y 패스만 `UnprojectIrradiance`로 irradiance를 출력한다.

## 패스 구성

```text
TemporalSH -> SeparableBlur X -> (transient Blur SH) -> SeparableBlur Y -> HistorySH + HistoryIrradiance
```

- X 패스가 full-res `R32G32B32A32_UINT` transient를 생성한다.
- Y 패스의 출력은 기존 `CSFinalBlur`와 동일하게 persistent history 슬롯이므로, 라이팅/히스토리 피드백 계약은 변하지 않는다.
- 토글 off면 기존 `CSFinalBlur` 단일 패스를 그대로 사용한다.

## 기존 경로와의 차이

- 기존 경로는 수렴 픽셀에도 항상 최소 반경 2로 블러를 적용했다. separable 경로는 수렴 픽셀을 블러하지 않으므로, 수렴 후 디테일이 더 선명하고 잔존 노이즈는 temporal 누적이 담당한다.
- 기존 경로는 surface-projected 탭(접평면 투영)이고 separable 경로는 화면 공간 탭이다. 경사가 급한 표면에서는 깊이/법선 가중치가 누설을 막는다.

## 검증

- 토글 on/off 화질 비교: 수렴 상태에서 동등 이상, 비수렴(카메라 이동 직후)에서 더 넓은 블러 커버리지.
- GPU 타이밍: steady-state에서 separable on이 기존 대비 같거나 낮아야 한다 (수렴 픽셀 early-out).
- 깊이/법선 불연속 경계에서 GI 번짐(누설) 없음.
- 디노이저 off 경로와 히스토리 무효화 동작은 변경 없음.
