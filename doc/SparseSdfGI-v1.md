# SparseSdfGI v1

이 문서는 최초 v1 프로토타입을 기록합니다. 현재 v2 계획과 후속 작업은 `doc/SparseSdfGI-v2.md`에서 추적합니다.

SparseSdfGI는 AMD FidelityFX Brixelizer 및 Brixelizer GI에서 영감을 받은 UncleRenderer의 독자적인 런타임 희소 SDF GI 실험입니다. 구현은 UncleRenderer 네이티브 코드이며 FidelityFX SDK 소스, 셰이더 코드, 데이터 테이블을 복사하지 않습니다.

## 목표

- 일반 정적 메시 삼각형으로부터 런타임 씬 공간 SDF를 구축한다.
- Brixelizer 스타일 간접 참조 레이아웃을 사용한다: 64^3 캐스케이드 브릭 맵이 512^3 SDF 아틀라스 내부의 8^3 브릭을 가리킨다.
- `brickId == 선형 캐스케이드 셀 인덱스`인 조밀한 단일 캐스케이드 매핑으로 시작한다.
- 희소 할당, 클립맵 스크롤링, AABB 트리 스킵 최적화 전에 디버그 레이 순회를 제공한다.

## v1 파이프라인

```text
SparseSdfGI SDF Clear
  -> 조밀 캐스케이드 브릭 맵 초기화
  -> SDF 아틀라스를 빈 거리로 클리어

SparseSdfGI Voxelize Model N
  -> 정적 일반 메시 삼각형
  -> SDF 아틀라스에서 정규화된 협대역 거리 근사

SparseSdfGI Trace
  -> 디버그 레이 순회 또는 오클루전 기반 확산 플레이스홀더 출력
  -> 간접 확산으로 선택적 합성
```

첫 구현은 성능보다 정확성과 가시성을 우선합니다. 확산 경로는 코사인 반구 방향을 샘플링하고 SDF에 맞은 레이를 어둡게 만드는 플레이스홀더이며, 히트 복사휘도(hit radiance) 또는 복사휘도 캐시가 추가되기 전까지는 완전한 GI보다 SDF 유도 AO에 가깝습니다. 스키닝 메시, ClusterDAG 패킹 지오메트리, 희소 브릭 할당, 정적/동적 병합, Eikonal 해법, 복사휘도/스페큘러 캐시는 다루지 않습니다.

## 참고 / 출처

- AMD FidelityFX SDK, Brixelizer / Brixelizer GI
- Copyright (C) 2024 Advanced Micro Devices, Inc.
- MIT 라이선스 적용

향후 SDK 코드, HLSL, 상수 또는 실질적인 코드 조각을 복사하는 경우, `ThirdPartyNotices.md`에 MIT 전문과 복사 범위를 포함한 항목을 추가하세요.
