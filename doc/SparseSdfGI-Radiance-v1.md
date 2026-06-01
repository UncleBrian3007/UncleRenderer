# SparseSdfGI Radiance v1

SparseSdfGI의 빌드 백본(`SparseSdfGI-v3.md`)과 별개로 추적되는 라이팅/복사휘도 트랙입니다. V3 빌드 파이프라인이 SDF 아틀라스와 브릭 메타데이터를 채우는 동안, Radiance 트랙은 그 SDF를 사용해 확산 GI 신호를 생성하는 트레이스/라이팅 경로를 다룹니다. V3 빌드 작업과 독립적으로 진행할 수 있습니다.

## Radiance v1 Diffuse Irradiance

Radiance v1은 기존 AO 형태 확산 트레이스를 첫 바운스 조사도(irradiance) 신호로 교체합니다. `CSDiffuseTrace`는 이제 SparseSdfGI 출력 텍스처에 조사도를 저장하며, 수신 표면 알베도를 곱하지 않습니다. 지연 합성 경로는 공통 확산 계약을 적용합니다.

```text
indirect diffuse = irradiance * albedo * (1 - metallic)
```

이는 환경광 및 ReSTIR 확산 GI 경로와 일치하므로 `Diffuse Indirect` 시각화는 사전 곱된 트레이스 출력이 아니라 보이는 표면 알베도에 의해 착색된 SparseSdfGI를 보여야 합니다.

v1 히트 라이팅 모델은 의도적으로 단순합니다.

- 미스 레이는 렌더러 환경 큐브를 직접 샘플링하여 레이 트레이싱 스카이 경로와 맞춘다.
- 히트 레이는 SDF 이동 거리로 히트 지점을 계산하고 SDF 그래디언트로 히트 노멀을 추정한다.
- 히트 조사도는 히트 노멀 기준 방향광 확산 바운스만 사용한다. 히트 지점에서 환경 큐브 스카이 항은 더하지 않는다(스카이를 더하면 과도하게 밝아져 제외).
- `SparseSdfGIBounceStrength`는 SparseSdfGI 조사도 기여를 스케일한다.
- `bSparseSdfGIUseHitLightingVisibility`는 히트 지점에서 방향광 쪽 가시성 레이를 선택적으로 쏜다. 기본값은 비활성으로, 광 누수 거동과 가시성 비용을 분리 관찰할 수 있다.

제약은 여전히 큽니다. 히트 표면 머티리얼 직접 조회는 없고, 복사휘도 캐시는 dense brick 평균값 1개짜리 v1 형태이며, 시공간 디노이즈도 없고, 픽셀당 단일 확률적 확산 레이를 여전히 사용합니다. Radiance v1은 신호 형태 마일스톤이지 프로덕션 품질 GI가 아닙니다.

## 현재 구현 요약 (코드 기준)

`CSDiffuseTrace`(픽셀당 1 스레드)의 실제 경로는 다음과 같습니다.

- trace 전에 visible GBuffer 표면을 `SparseSdfGI Radiance Clear -> Inject -> Resolve Temporal -> Copy History`로 처리해 dense brick radiance cache를 갱신한다. v1 cache는 화면 픽셀의 `albedo * direct diffuse * shadow visibility * (1 - metallic)`를 brick별 정수 누적/평균으로 저장한다.
- 표면 노멀 반구에서 코사인 가중 확산 레이 **픽셀당 1개**를 샘플(`SampleHemisphereCosine`, 프레임별 난수)하여 SDF 가시성을 트레이스한다.
- **미스**: `EvaluateSparseSdfGISky(traceDirection)` 환경 큐브 스카이를 조사도로 사용한다.
- **히트**: SDF 이동 거리로 히트 지점을, SDF 그래디언트(`ComputeSdfNormal`)로 히트 노멀을 구하고, 히트 brick의 radiance cache를 우선 샘플한다. cache가 비어 있으면 기존 **방향광 1개의 직접 바운스** `LightColor * LightIntensity * saturate(NdotL) * visibility`로 fallback한다. 미스용 스카이 값(`EvaluateSparseSdfGISky(traceDirection)`)은 히트 시 cache/direct bounce로 덮어쓰며, **히트 지점에서 환경 큐브 스카이 항을 더하지 않는다**(이전의 `skyBounce(hitNormal) + directBounce`는 과도하게 밝아져 제거됨). `UseHitLightingVisibility`가 켜진 경우에만 fallback direct bounce의 방향광 가시성 레이를 1개 더 쏜다.
- 출력은 `irradiance * Intensity * BounceStrength`이며 수신 표면 알베도는 곱하지 않는다(합성 단계에서 곱함).

즉 v1은 **단일 확률적 레이 + visible surface brick cache + brick 단위 temporal persistence**라 표면 색 기여와 간단한 offscreen 유지가 생기지만, screen-space 디노이즈가 없어 노이즈가 크고 occluded 표면 충실도는 낮다.

## 향후 작업 (Radiance 로드맵)

v1은 신호 형태 마일스톤이므로, 남은 작업은 정상 상태 비용보다 **Surface Radiance Cache**(히트 라이팅 충실도·멀티 바운스)와 **노이즈/누적**이라는 두 품질 축이 결정합니다. 가장 큰 레버는 radiance cache와 시공간 누적입니다.

### Surface Radiance Cache (BrixelizerGI 스타일, 우선순위 상)

가장 큰 품질 레버이며 히트 라이팅 충실도·멀티 바운스·복사휘도 캐시를 하나로 묶는 축입니다. BrixelizerGI는 SDF hit마다 mesh triangle/material을 직접 조회하지 않고, 화면에 보이는 표면의 lit 출력(직전 프레임 lit output, screen probe)을 월드공간 radiance cache에 주입한 뒤, SDF ray가 hit한 지점에서 그 cache를 샘플합니다. 이 방식이 per-hit material lookup보다 우선입니다 — direct lookup은 voxel/brick에 material payload를 들고 hit마다 풀 라이팅을 돌려야 해서 비싸고, cache가 있으면 대체로 불필요합니다. 또한 cache 주입은 메인 lit 패스 출력을 그대로 담으므로 방향광뿐 아니라 점/스폿/에미시브 등 모든 광원 기여가 자연히 포함됩니다(별도 GI용 광원 확장이 대부분 불필요).

단계:

1. **Visible Surface Radiance Injection**: GBuffer/lit 출력 기반으로 brick radiance cache를 채운다. 화면 픽셀 → 월드 위치 → brick 매핑에서 여러 픽셀이 같은 brick에 들어가므로 atomic 누적/평균이 필요하다. visible surface의 albedo·direct lighting이 cache에 반영된다. 현재 v1은 GBuffer의 albedo/normal/metallic, 방향광 direct diffuse, 사용 가능한 경우 ray-traced shadow mask로 시작한다.
2. **SDF Hit Radiance Sampling**: `CSDiffuseTrace` hit 지점에서 brick radiance cache를 샘플한다. cache miss(아직 화면에 안 보인 brick) 시 기존 방향광 direct bounce 또는 sky fallback을 사용한다. 현재 v1은 brick당 평균 RGB 1개만 저장하므로 방향성은 없다.
3. **Temporal Reuse**: 직전 프레임 cache를 유지해 화면 밖 또는 이전에 보였던 표면 기여를 보완한다. 현재 구현은 visible brick은 이번 프레임 injection을 그대로 사용하고, 이번 프레임에 보이지 않은 brick만 직전 cache를 decay한다. ImGui의 `SDF GI Temporal Reuse` 옵션으로 켜고 끌 수 있으며, 꺼진 상태에서는 history를 샘플하거나 갱신하지 않는다. SDF cache signature가 바뀌어 SDF가 재빌드되면 radiance history도 무효화한다.
4. **Probe / SH 확장**: brick 단일 RGB cache에서 probe 또는 SH 기반 directional radiance로 확장해 방향성 있는 간접광을 표현한다.
5. **Direct Material Lookup (후순위)**: triangle/material payload 기반 hit material lookup은 cache로 메우지 못하는 부분의 정확도 보강용 후속 단계로 둔다.

**에너지 보존 주의**: cache는 화면 기여를 다시 cache로 먹이는 피드백 루프라, 직전에 제거한 *히트 지점 과도한 밝아짐*이 재발하기 가장 쉬운 지점이다. 주입·누적 단계에서 클램프·에너지 정규화로 멀티 바운스가 발산하지 않도록 한다.

알려진 한계: screen-visible 기반이라 최초로 보이는 offscreen 표면은 cache가 비어(2단계 fallback) temporal/probe로 점진 보완된다. 현재 temporal은 brick 평균 RGB와 confidence만 유지하므로 방향성은 없고, 오래 보이지 않은 brick은 decay로 사라진다. Probe/SH가 없으면 light leak과 기여 누락이 남는다.

### 노이즈 저감 / 누적 (우선순위 상)

- **시공간 디노이즈**: 픽셀당 단일 확률적 코사인 레이 출력에 temporal reprojection + spatial filter를 적용하거나, ReSTIR GI 스타일 reservoir 재사용으로 분산을 줄인다. 상세 설계·구현 계획과 BrixelizerGI 기반 순서 결정(디노이저 vs Probe/SH)은 [SparseSdfGI-Radiance-Denoise-v1.md](SparseSdfGI-Radiance-Denoise-v1.md) 참고.
- **샘플 시퀀스 개선**: 블루노이즈/소볼 등으로 픽셀·프레임 간 상관을 낮춰 노이즈를 분산시킨다.

### 가시성 / 광 누수

- `bSparseSdfGIUseHitLightingVisibility` 기본 활성화 여부와 비용/품질 트레이드오프를 결정한다.
- 얇은 지오메트리에서의 SDF 가시성 누수를 편향(bias)·표면 두께(`SurfaceThicknessVoxels`) 튜닝으로 완화한다.

### 통합 / 검증

- ReSTIR 확산 GI·환경광 경로와의 공통 확산 계약(`irradiance * albedo * (1 - metallic)`) 일관성을 지속 검증한다.
- `Diffuse Indirect` 시각화가 알베도 착색 후 신호를 정확히 보여주는지 회귀 확인한다.

## 참고

- 빌드 파이프라인(분할 참조 바이닝, 점유 브릭 solve, 다중 캐스케이드 등)은 `SparseSdfGI-v3.md`에서 별도로 추적한다.
- Surface Radiance Cache 로드맵은 AMD FidelityFX BrixelizerGI의 radiance cache / screen probe 구조에서 영감을 받았다. FidelityFX SDK 소스, 셰이더 코드, 상수 테이블은 복사하지 않는다.
