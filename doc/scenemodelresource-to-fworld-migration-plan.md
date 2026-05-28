# SceneModels to FWorld Migration Plan

작성일: 2026-05-27

## 진행 현황 (2026-05-27)
- 완료: Phase 1, Phase 2, Phase 3, Phase 4, Phase 5, Phase 6
- 비고:
	- FWorld가 SceneModels를 실소유하도록 전환 완료
	- FRenderer의 임시 `SceneModels` alias 제거 완료
	- 로더는 OutWorld가 주어지면 World의 SceneModels에 직접 적재
	- 렌더 패스(ObjectId / DeferredBasePass / PathTracing / RayTracingShadow / SSR / RayTracingRuntime)의 직접 `Owner.SceneModels` 접근을 World 경유로 정리

## 목표
- FSceneModelResource 소유권을 FRenderer에서 FWorld로 이전한다.
- 기존 데이터 주도 렌더 경로를 깨지 않도록 점진적 브리지 방식으로 전환한다.
- ObjectId, ClusterDAG, 상수버퍼 인덱스 계약을 우선 보호한다.

## 범위
- 포함: SceneModels ownership 이전, 접근 경로 정리, 로더 write-path 이전
- 제외: FMeshMaterial 공유/디듀프(별도 후속 작업)

## 단계별 계획

### Phase 0. 인덱스 계약 고정
1. SceneModels 인덱스를 외부 계약으로 사용하는 지점(ObjectId, SectionModelIndices, constant buffer offset)을 명시한다.
2. 전환 완료 전까지 append-only 정책을 유지해 인덱스 안정성을 보장한다.

### Phase 1. 브리지 단계(동작 동일)
1. FRenderer.GetSceneModels / GetSceneModelsMutable를 World 경유로 라우팅한다.
2. 렌더 경로가 World 기반 접근으로 넘어갈 수 있도록 일시적 호환 경로를 둔다.

### Phase 2. Read-path 전환
1. Application(ObjectId readback/selection) 조회 경로를 World 경유로 정리한다.
2. ObjectId pass와 디버그/UI 경로의 모델 조회를 World 기반 접근으로 통일한다.
3. 선택 오브젝트 이름/인덱스/바운딩박스 출력 회귀를 확인한다.

### Phase 3. Render-path 전환
1. Deferred/Forward/RT/ClusterDAG의 SceneModels 접근을 World 경유로 이동한다.
2. SortedIndices와 원본 ModelIndex 매핑 계약을 유지한다.
3. SceneModelVisibility / SceneModelSkinningVisibility 동기화 규칙을 재검증한다.

### Phase 4. Loader write-path 이전
1. SceneModelResourceLoader가 OutWorld 내부 SceneModels 저장소를 직접 채우도록 변경한다.
2. OutModels 인자 의존을 제거하거나 최소화해 단일 소유 경로로 정리한다.

### Phase 5. Ownership flip
1. FWorld를 SceneModels 실소유자로 승격한다.
2. mutable 접근을 로더/셋업 전용 API로 축소한다.

### Phase 6. 호환 경로 정리
1. 임시 브리지/호환 getter를 제거한다.
2. 직접 SceneModels 참조를 단계적으로 World API 경유로 정리한다. (완료)
3. 핸드오프 문서와 UI 설명을 최종 구조 기준으로 업데이트한다. (완료)

## 핵심 리스크
1. ObjectId -> ModelIndex 역매핑 불일치
2. ClusterDagSectionModelIndices의 인덱스 무효화
3. Draw constant buffer index 계산 불일치
4. 렌더 패스별 SceneModels/Visibility 벡터 크기 불일치

## 검증 체크리스트
1. Debug x64 빌드 통과
2. 씬 로드 후 World Objects 개수/섹션 매핑 일치 확인
3. ObjectId 클릭 선택 회귀(이름/인덱스/하이라이트)
4. Deferred/Forward 결과 비교
5. ClusterDAG 섹션 렌더 회귀 확인
6. RT TLAS 인스턴스/가시성 결과 비교
7. 최종 단계에서 Renderer 직접 SceneModels 참조 0건 확인

## 대상 파일
- Source/World/World.h
- Source/Render/Renderer.h
- Source/Render/Renderer.cpp
- Source/Render/SceneModelResourceLoader.cpp
- Source/Core/Application.cpp
- Source/Render/ObjectId.cpp
- Source/Render/Deferred/ClusterDagRuntime.cpp
- Source/Render/DeferredRenderer.cpp
- Source/Render/ForwardRenderer.cpp
