# SparseSdfGI Radiance Denoise v1 (계획)

빌드 백본(`SparseSdfGI-v3.md`), 라이팅/캐시(`SparseSdfGI-Radiance-v1.md`)와 별개로, **노이즈 많은 per-pixel GI를 스크린공간에서 디노이즈**하는 작업의 설계와 구현 계획을 다룬다. AMD FidelityFX BrixelizerGI의 디노이저 구조에서 영감을 받되 SDK 소스/셰이더/상수는 복사하지 않는다. (구현 전 계획 문서)

## 현재 노이즈의 출처

`CSDiffuseTrace`는 픽셀당 코사인 확산 레이 1개로 brick radiance cache를 1회 샘플한다. brick cache 자체는 다수 화면 픽셀로 공간 평균 + temporal 유지되어 **매끄럽다**. 노이즈는 **픽셀마다 단일 레이가 서로 다른 brick을 확률적으로 선택**(+ frame별 hit/miss 전환)하는 **per-pixel 샘플링**에서 온다. 즉 cache 표현이 아니라 sampling 분산 문제다.

## 순서 결정: 디노이저 vs Probe/SH

"Probe/SH 확장부터 해야 하지 않나?"에 대한 정리:

- **brick-SH(브릭 캐시 방향성, Radiance 로드맵 4단계)는 노이즈를 줄이지 않는다.** 픽셀당 여전히 단일 레이 1샘플이며, SH는 방향별 값 차이를 키워 **per-pixel 분산을 오히려 늘릴 수 있다.** 먼저 해도 화면이 noisy해 개선을 평가할 수 없다.
- **노이즈를 줄이는 건 디노이저(temporal accumulation + spatial filter)** 이며, cache가 RGB든 SH든 무관하게 동작한다. 따라서 **"깨끗한 GI를 보는" 선행 조건은 디노이저다.**
- 다만 **screen probe는 의미가 다르다.** BrixelizerGI의 screen probe는 희소 스크린 위치에서 다수 레이를 트레이스해 SH 프로브를 만들고 보간하므로, 그 자체가 (a) 방향성(SH) + (b) 노이즈 저감(다수 레이 + 보간 + temporal)을 **동시에** 준다. BrixelizerGI에서는 probe/SH와 디노이즈가 한 시스템이다. → "probe부터"라는 직관은 **screen-probe 경로를 택할 때** 맞고, brick-SH 단독을 먼저 하라는 뜻은 아니다.

확정 경로 (Phase 0 → Phase 1 → Road B):

1. **Phase 0 — 스크린공간 temporal accumulation**: per-pixel 출력에 reproject + disocclusion + temporal + AABB clamp.
2. **Phase 1 — spatial bilateral blur**: temporal 후 잔여 노이즈 평탄화. **여기까지 Phase 0+1 = per-pixel 경로의 완성된 SVGF-lite 디노이저** → 깨끗하고 평가 가능한 baseline 확보.
3. **Road B — screen probe + SH**: per-pixel 샘플링 front-end를 희소 screen probe로 교체(방향성 + 더 적은 레이). Phase 0의 temporal은 probe로 relocate, Phase 1의 blur는 최종 spatial 패스로 유지.

**왜 Phase 1을 중간에 하나**: Phase 0+1의 reproject/disocclusion/temporal/clamp/bilateral 로직은 Road B에서 **버리지 않고 재배치**된다(probe·probe-output 위에 동일 적용). 먼저 단순한 per-pixel 경로로 디노이즈 체인을 끝까지 검증·튜닝해두면, Road B의 큰 리팩터에서 "probe 디버깅 + 디노이저 디버깅"을 동시에 하지 않아도 된다. 즉 Phase 0+1은 버리는 투자가 아니라 **Road B가 재사용하는 디노이즈 백엔드**다.

**brick-SH(로드맵 4단계 원안)는 Road B에서 대체로 불필요**: screen probe는 RGB brick을 여러 방향으로 트레이스해 SH를 만들므로(서로 다른 레이 방향이 서로 다른 brick을 hit), **brick이 RGB여도 probe SH가 방향성 indirect를 제공**한다. brick 자체의 방향성 방출은 더 미세한 효과라 후순위.

## BrixelizerGI 디노이저 구조 (참고)

패스 순서: `GenerateDisocclusionMask` → (`EmitPrimaryRayRadiance`) → `ReprojectScreenProbes`/`ReprojectGI` → `InterpolateScreenProbes`(temporal accumulation) → `BlurGI`(spatial).

핵심 기법:

- **Disocclusion mask**: motion vector로 reproject한 history를 기하 일치도로 거부. 대략 `exp(-|1 - max(0, dot(N, N_hist))| * 1.4) * exp(-|worldPos - worldPos_hist| / depth)` + depth 비교.
- **Temporal reprojection**: `history_uv = uv + motionVector`, 화면 밖 또는 disocclusion이면 reject.
- **Temporal accumulation**: 지수 블렌드(`lerp(history, current, ~0.25)`, history 없으면 1.0) 또는 sample-count 가중(최대 ~64). **neighborhood AABB color clamp**(이웃 색 박스로 history를 클램프, factor 0.3~0.5)로 ghosting/firefly 제거. temporal variance(`(|L_hist - L| / max)²`)로 변화 큰 곳은 current 가중↑.
- **Spatial bilateral blur**: separable X/Y, 노멀·깊이 edge-stopping.
- half-res downsample + upsample로 비용 절감.

## 구현 계획

### Phase 0 — 스크린공간 Temporal Accumulation (선행, 필수)

- 신규 persistent texture: `DiffuseGIHistory`(직전 프레임 디노이즈 출력), reproject 검증용 직전 depth/normal(기존 history 버퍼 재사용 가능 여부 확인).
- 입력: 현재 프레임 `DiffuseGI`(noisy), motion vector(TAA용 기존 것 재사용), 현재/직전 depth·normal.
- 패스 `CSDenoiseTemporal`:
  1. `history_uv = uv + motionVector`.
  2. disocclusion: 화면 밖 / 노멀·깊이 불일치 → history reject.
  3. `out = valid ? lerp(history, current, alpha) : current` (alpha ~0.1~0.25, 또는 sample-count 누적으로 초반 빠르게 수렴 후 안정).
  4. **neighborhood AABB clamp**(현재 프레임 3×3 평균/분산 박스로 history 클램프) → ghosting/firefly 억제.
- 출력 → 디노이즈된 `DiffuseGI` + `DiffuseGIHistory` 갱신.
- history invalidation: resize / SparseSdfGI toggle / SDF rebuild / 카메라 cut 시.

### Phase 1 — Spatial Bilateral Blur

- 패스 `CSDenoiseBlurX` / `CSDenoiseBlurY`(separable), 노멀·깊이 가중 edge-stopping.
- temporal 후 잔여 노이즈 평탄화. 누적 샘플 수가 낮은 픽셀(최근 disocclusion)에서 블러 반경↑.

### Phase 2 — Road B: Screen Probe + SH

per-pixel 1-ray 샘플링을 희소 screen probe로 교체한다. probe가 RGB brick cache를 다수 방향으로 트레이스해 SH를 만들고, full-res로 보간해 GI를 생성한다. Phase 0 temporal은 probe에, Phase 1 blur는 최종 spatial 패스로 유지된다.

세부 단계:

1. **Probe placement (spawn)**: 화면을 타일(예: 8×8)로 나눠 타일당 1 probe 배치. probe 대표 픽셀은 타일 내 depth/normal 기준 선택, 프레임 간 jitter로 커버리지 보완.
2. **Probe trace**: probe당 다수 코사인 레이(예: 32~64)를 brick cache(또는 sky)로 트레이스, hit radiance를 **SH(L1 = 4계수 × RGB)** 로 투영. 다수 레이라 probe 자체가 저노이즈.
3. **Probe reproject + temporal**: 직전 프레임 probe SH를 motion vector로 reproject, disocclusion으로 거부, sample-count 가중 누적(**Phase 0 로직 relocate**). probe당 누적이라 빠르게 수렴.
4. **Interpolate to full-res**: 각 픽셀이 인접 probe들을 depth/normal 가중 보간 후 픽셀 노멀 방향으로 SH 평가 → per-pixel diffuse GI. (`CSDiffuseTrace`의 per-pixel brick 직접 샘플을 대체)
5. **Spatial blur**: **Phase 1의 bilateral blur를 최종 패스로** 적용.
6. (선택) **brick directional cache(SH)**: probe SH로 부족한 brick 자체 방향성 방출이 필요할 때만. 후순위.

신규 리소스: `ProbeRadianceSH`(현재/history), `ProbeHeader`(위치·유효성), (재사용) `DiffuseGIHistory`.
신규 패스: `CSSpawnProbes`, `CSTraceProbes`, `CSReprojectProbes`(temporal), `CSInterpolateProbes`. (Phase 0/1 패스는 재배치/유지)

## 리소스 / 패스 요약

- Phase 0/1 텍스처: `DiffuseGIHistory`(+ 필요 시 history depth/normal, downsampled).
- Phase 0/1 패스(각 패스 b2 bindless layout만 사용, root signature 미확장): `CSDenoiseTemporal`, `CSDenoiseBlurX`, `CSDenoiseBlurY`.
- Road B 추가 리소스/패스: `ProbeRadianceSH`(현재/history), `ProbeHeader`; `CSSpawnProbes`, `CSTraceProbes`, `CSReprojectProbes`, `CSInterpolateProbes`.
- 의존: motion vector(렌더러 기존), GBuffer normal/depth, 직전 프레임 depth/normal.

## 리스크 / 메모

- motion vector 정확도: 현재 정적 단일 캐스케이드라 카메라 모션 위주. 동적 지오메트리는 후속.
- AABB clamp 강도 튜닝: 약하면 ghosting, 강하면 떨림.
- per-pixel 디노이저는 이후 screen-probe로 전환 시 일부 재작업이 생기나, Phase 0의 reproject/temporal/clamp 로직은 재사용 가능.
- 에너지: 디노이저는 신호를 평활화할 뿐이며, GI를 cache로 재주입하지 않으므로(스크린공간) 직전에 고친 과밝음/피드백 루프와 무관하다.

## 참고

- AMD FidelityFX BrixelizerGI 디노이저(disocclusion / reproject / temporal accumulation + AABB clamp / bilateral blur / screen probe) 구조에서 영감을 받았다. FidelityFX SDK 소스, 셰이더 코드, 상수 테이블은 복사하지 않는다.
- 빌드 파이프라인: `SparseSdfGI-v3.md`. 라이팅/캐시: `SparseSdfGI-Radiance-v1.md`.
