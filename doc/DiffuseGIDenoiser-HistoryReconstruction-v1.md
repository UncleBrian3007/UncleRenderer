# Diffuse GI 디노이저 히스토리 재구성 v1

## 요약

Diffuse GI 디노이저가 활성화된 상태에서 실루엣 및 곡률이 높은 픽셀에 안정적인 검은 점이 나타났다 (SparseSdfGI 스크린 프로브 입력, duck 씬). 프로브 보간 입력은 정상이었으며 (신뢰도 디버그 뷰 균일), 검은 점은 디노이저 내부 `CSHistoryReconstruction`에서 발생했다.

## 근본 원인

`CSHistoryReconstruction`은 `HistoryCount < 10`인 픽셀에 대해 `TemporalSH`를 밉 필터링된 SH로 교체한다. 세 가지 결함이 결합되어 검은 출력이 발생했다:

1. 깊이 가중치가 절대 월드 단위 차이를 사용했다 (`exp(-abs(CurrentDepth - DepthMip))`). 깊이 밉이 하늘 깊이 `0`을 실루엣 셀에 평균화해, 엣지에서 모든 탭 가중치가 ~0으로 붕괴됐다.
2. 폴백이 없었다: 모든 탭 가중치가 소멸되면 정규화가 ~0의 SH를 생성하고, 이것이 유효한 현재 프레임 SH를 **덮어썼다**.
3. TAA 지터가 매 프레임마다 실루엣 픽셀을 표면과 하늘 사이에서 전환시켜, 시간적 오클루전 테스트가 계속 히스토리를 거부하고 `HistoryCount`가 1에 머물러 있어 실루엣 픽셀이 이 경로에서 빠져나오지 못한다.

재구성은 더 나은 추정치를 향해서만 보정해야 한다. 기존 코드는 추정치가 없을 때 더 나쁜 값으로 교체했다.

## 변경 사항

### CSHistoryReconstruction (`Shaders/GIDenoiser/DiffuseGIDenoiser.hlsl`)

- 전체 탭 가중치가 `<= 1e-4`일 때 조기 반환으로 기존 `TemporalSH`를 유지한다.
- 깊이 가중치를 상대적으로 변경: `exp(-abs(CurrentDepth - D) / DepthSigma)`, `DepthSigma = max(CurrentDepth * 0.1, 1e-3)`. 가까운 표면은 엄격하게, 먼 표면은 허용적으로 유지된다.
- 재구성된 SH가 현재 `TemporalSH`를 교체하는 대신 블렌딩한다:

```hlsl
ReconstructAmount = saturate(1 - CountRatio) * saturate(Total * 4)
TemporalSH = LerpSh(CurrentTemporal, Reconstructed, ReconstructAmount)
```

히스토리 카운트가 낮으면 재구성 방향으로 당기고, 탭 신뢰도가 낮으면 현재 SH 방향으로 당겨온다. 이를 통해 `CSFinalBlur`를 통해 `HistorySH`로 피드백되는 저품질 밉 데이터도 차단한다.

### 하늘 인식 밉 생성 (최소 비용 변형)

두 SPD 리듀서가 이전에는 하늘을 모든 경계 셀에 평균화했다. 이제 리덕션 과정에서 유효성 가중치를 전달하여 유효한 값만 평균화한다:

- `RestirGiLinearDepthMipGenSpd.hlsl`: 유효성은 `depth > 0`이며, 미사용 `.y` 채널에 전달된다. 완전히 하늘인 셀은 깊이 `0`을 저장하고, 상대적 깊이 가중치가 자연스럽게 이를 거부한다.
- `RestirGiMipGenSpd.hlsl`: SPD 값 타입은 `FWeightedSh { FPackedSh, Weight }`이다. 유효성은 `packed != 0`이며, 하늘 픽셀은 `CSPreBlur`/`CSTemporalAccumulation`에서 정확히 0인 SH를 저장하므로 별도의 깊이 바인딩이 필요 없다. 기하학적으로 진짜 0인 SH도 평균에서 제외되는데, 이는 허용 가능한 최소 비용 바이어스다.

저장된 밉 텍셀의 포맷은 변경되지 않으며, 가중치는 리덕션 내부에서만 존재한다.

### 데드 코드 제거

`DiffuseGIDenoiser.hlsl`의 `CSGenerateShMips`와 `CSGenerateLinearDepthMips`는 미사용 상태였으며 (PSO가 SPD 변형을 컴파일), 기존의 하늘 오염 평균화를 사용했다. 제거됨.

## 검증

- Duck 씬, SparseSdfGI + 스크린 프로브, 디노이저 켬: 실루엣/주름 픽셀에 검은 점 없음 (조기 반환 가드만으로도 실험적으로 확인됨).
- 디노이저 켬/끔 비교: 실루엣 밝기가 어두워지지 않고 일치해야 함.
- 깊이 불연속을 가로지르는 카메라 이동: 비가림 영역이 어두운 프린지 없이 밉에서 채워져야 함.

## 향후 과제

- `ComputeGeometryWeight` (`exp(-abs(depth diff))`)와 `ComputeSurfaceGeometryWeight` (평면 허용 오차 고정값 1.0 월드 단위)는 여전히 씬 스케일에 의존적이다. 깊이 비례 시그마로 통일해야 한다.
- `DepthSigma`가 밉 풋프린트를 무시한다. 급경사에서 유효 탭이 소멸될 경우 `(1 << ComputedMip)` 비례 항을 추가해야 한다.
- 밉 셀당 명시적 min/max 깊이를 전달하면 zero-as-invalid 휴리스틱을 실제 유효성 구간으로 대체할 수 있다.
