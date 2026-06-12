# Diffuse GI 디노이저 Lightweight 모드 v1

## 요약

- FFX BrixelizerGI의 스크린 공간 디노이즈는 ReprojectGI(시간 누적)와 BlurX/BlurY(separable 가변 반경) 두 단계뿐이다.
- `DiffuseGIDenoiserLightweight`(기본값 true)가 켜지면 같은 형태의 경량 파이프라인으로 동작한다:

```text
기존:     PreBlur -> Temporal -> SH Mip(SPD) -> Depth Mip(SPD) -> HistoryReconstruction -> FinalBlur
경량:     Temporal -> SeparableBlur X -> SeparableBlur Y
```

- 끄면 기존 전체 파이프라인(PreBlur + SPD 밉 + HistoryReconstruction + 4-tap Poisson FinalBlur)을 그대로 사용한다.
- 토글 변경 시 디노이저 히스토리를 무효화한다.

## SeparableBlur 커널

`CSSeparableFinalBlur` 하나가 X/Y 두 방향을 처리하며, 방향은 `BlurDirectionY` 상수로 선택한다.

- 반경: `Radius = 8 * smoothstep(1 - HistoryCount / 32)` 픽셀. 수렴(count 32)에서 0, 신규 픽셀에서 8.
- `Radius < 0.5`면 소스 SH를 그대로 통과시킨다 (early-out). steady-state 비용이 탭 비용에서 복사 비용으로 줄어든다.
- 탭: 방향당 `{-2, -1, +1, +2} * (Radius / 2)` 4탭 + 중심. 두 패스 합산 커버리지는 `(2 * Radius + 1)^2` 분리 커널이다.
- 가중치: 가우시안(`exp(-0.5 * i^2)`) × 상대 깊이(`exp(-|d - d'| / max(d * 0.1, 1e-3))`) × 법선(`pow(dot, 32)`).
- 마지막(Y) 패스 여부는 `OutHistoryIrradianceIndex` 유효성으로 판별하고, Y 패스만 `UnprojectIrradiance`로 irradiance를 출력한다.
- X 패스가 full-res `R32G32B32A32_UINT` transient를 생성하며, Y 패스의 출력은 기존과 동일한 persistent history 슬롯이라 라이팅/히스토리 피드백 계약은 변하지 않는다.

## 경량 모드의 트레이드오프

- Temporal 패스의 현재 프레임 입력이 PreBlur 결과 대신 raw `InputSH`다. 1-spp 노이즈는 separable blur의 비수렴 최대 반경(8px)이 담당한다.
- HistoryReconstruction이 없으므로 disocclusion 픽셀은 밉 채움 없이 1프레임 데이터에서 시작한다. count가 낮은 동안 separable 반경이 크게 유지되어 공간적으로 가려준다.
- 수렴 픽셀은 블러를 받지 않는다(early-out). 기존 경로는 수렴 후에도 최소 반경 2 블러를 적용했으므로, 수렴 후 디테일은 경량 모드가 더 선명하고 잔존 노이즈는 temporal 누적이 담당한다.

## 검증

- 토글 on/off 화질 비교: 수렴 상태 동등 이상, disocclusion(카메라 회전/이동)에서 기존 대비 노이즈 정리 속도 확인.
- GPU 타이밍: 경량 on에서 PreBlur + SPD 2회 + HistoryReconstruction이 사라지고 separable 2패스만 남아 디노이저 비용이 내려가야 한다.
- 깊이/법선 불연속 경계에서 GI 번짐(누설) 없음.
- 실루엣 검은 점 회귀 없음 (HistoryReconstruction 자체가 경량 경로에 없음).
