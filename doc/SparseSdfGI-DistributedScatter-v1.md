# SparseSdfGI Exact Direct Distributed Scatter v1

> 이 문서는 "Exact Direct Distributed Scatter 계획 v2"에 대한 **설계 + 리뷰 반영 + 후속 단계 의존성**을 통합한 것이다. 리뷰에서 합의된 보강(High 2건)을 설계 결정으로 못박았다.

## 배경 / 동기

현재 `ExactSharedBorder` SDF 빌드는 **gather solve**(브릭마다 삼각형 참조 리스트를 모아 voxel별 거리 계산)다. 이 방식의 두 비용:

1. **큰-삼각형 홀**: 삼각형이 너무 많은 브릭에 걸치면(`SPARSE_SDF_GI_MAX_TRIANGLE_BRICK_REFERENCES = 4096` 초과) **통째로 drop** → 표면 누락 ([SparseSdfGI.hlsl](../Shaders/SparseSdfGI/SparseSdfGI.hlsl) `CSEmitTriangleReferences`). 과거 per-voxel scatter의 `MaxTriangleVoxelSpan` 홀을 gather로 옮긴 산물.
2. **7복셀 인플레이션**: gather라 모든 voxel이 자기 삼각형을 봐야 해 `referenceBand = 7 * VoxelSize`로 부풀림 → 참조 fan-out·메모리·REFOVF.

FFX Brixelizer는 **distributed direct scatter**(삼각형 → 자기가 덮는 brixel에 거리 atomic-min, 작업을 스레드에 분산)로 **두 문제를 동시에 회피**한다. 본 작업은 `ExactSharedBorder`의 gather solve를 FFX식 scatter로 교체한다.

## 핵심 결정

- `ExactSharedBorder` 선택 시 **항상 direct scatter** 사용 (별도 런타임 A/B 토글 없음).
- `LegacyEikonal` build mode는 비교/폴백용으로 **유지** (제거 대상은 `ExactSharedBorder`의 gather solve 경로뿐).
- **dense `512^3` R32_UINT scratch(512 MiB) 금지**: touched brick만 compact 할당한 `MaxScatterBricks * 8^3` R32_UINT scratch 사용.
- 단일 `64^3` 캐스케이드 유지 (멀티 캐스케이드는 후속 단계).

## 메모리 레이아웃

- **추가 dense full scratch 금지** (`512^3 * 4 = 512 MiB` R32_UINT 만들지 않음).
- direct path scratch:
  - dense touched flags: `64^3` uint (약 1 MiB).
  - compact brick list/map: `MaxScatterBricks` 단위.
  - brick-local SDF scratch: `MaxScatterBricks * 8^3` uint.
- 최종 `SdfAtlas`(R16_UNORM, persistent)는 그대로 두고, finalize 패스에서 scratch uint → float 변환 복사.
- 예산 `SparseSdfGIMaxScatterBricks` 추가. 기본 `65536` → scratch `65536 * 512 * 4 = 128 MiB`.
  - 예산 변경은 SDF/radiance/irradiance cache, probe history를 invalidate.

## 패스 순서 (`ExactSharedBorder`)

rebuild 프레임에만 실행(캐시됨). 기존 gather emit/solve 대체.

```text
1.  CSInitDistributedScatterBuild   SDF far clear, metadata/cascade-map/counters/touched flags clear
2.  CSPrepareScatterJobs            section 삼각형 → world tri + exact shared-border sample AABB + sample count (긴 루프 없음)
3.  CSScanScatterJobCounts          per-triangle sample count prefix scan (group 256, 2-level)
4.  CSBuildScatterSampleArgs        total sample count → indirect dispatch args
5.  CSMarkScatterTouchedBricks      [indirect over samples] global sample id → (job, local sample) → touched logical brick flag set
6.  CSAllocateScatterBricks         [dense 64^3] touched brick → compact physical id; CascadeBrickMap[logical]=phys, ScatterBrickList[phys]=logical
7.  CSBuildScatterBrickArgs + CSClearScatterBrickStorage   allocated brick scratch를 far uint로 clear
8.  CSScatterSdfSamples             [indirect over samples] triangle distance 계산 → allocated brick local sample에 InterlockedMin
9.  CSFinalizeScatterBricks         [brick당 512 threads] scratch uint → normalized float → SdfAtlas, occupied local AABB reduce → BrickMetadata
10. (기존) Build Trace Hierarchy Bottom/Top + Stats Present
```

핵심: **2~5는 작업을 sample 단위로 분산**한다 — 스레드 1개가 sample 1개를 맡으므로 큰 삼각형이 여러 스레드에 쪼개져 처리됨(= per-triangle 무제한 작업/drop 제거).

## 인코딩

- scratch far value: `0x00ffffffu`.
- `EncodeScatterDistanceUint(d) = round(saturate(d / GetSdfWorldDistanceScale()) * 0x00ffffffu)` → `InterlockedMin` (작을수록 가까움).
- finalize: `SdfAtlas[atlasCoord] = encoded / 0x00ffffff` ( = 0..1, far=1.0, 기존 SDF 의미와 정합).
- trace/sampling decode는 기존 `DecodeSdfWorldDistance` 그대로.

## 리뷰 반영 — 반드시 지킬 2가지 (High)

### H1. Shared-border 일관성: sample은 **per-brick-local-voxel**로 열거
exact shared-border는 **brick N local7 == brick N+1 local0**(같은 월드 좌표, 물리적으론 별도 저장)이어야 seam이 없다. scatter가 한 월드 voxel을 **유일 브릭 하나**에만 쓰면 경계 복제본 중 하나만 채워져 **seam 재발**.

→ sample 열거를 **(삼각형, 브릭, 브릭-local voxel)** 단위로 한다. 삼각형이 닿는 **양쪽 브릭의 local7/local0을 각각** scatter해 두 복제본이 같은 min을 받게 한다. (gather가 양쪽 브릭에서 각자 계산하던 것과 동일 효과.) **`CSMarkScatterTouchedBricks`/`CSScatterSdfSamples`의 sample = per-brick-local로 정의.**

### H2. Scatter 밴드 두께 = sample count·trace correctness 동시 결정
- **너무 두꺼우면**(full AABB / 7복셀): sample count = O(AABB 부피 × 밴드) → 수천만 sample + atomic 폭주.
- **너무 얇으면**: sphere-trace `maxStep(2복셀)` 안에 거리가 없어 overstep/구멍.
- → **밴드 = `maxStep + 마진 ≈ 2~3복셀`** (상수 `ScatterBandVoxels ≈ 3`). 그 밖은 far로 saturate. 이게 **sample 폭주를 막으면서 conservative SDF 유지**하고, 동시에 **7복셀 인플레이션 축소**를 실현한다.

## 리뷰 — 강점과 나머지 보강

**강점**: sample 분산(scan+indirect)으로 큰-삼각형 drop 원천 제거(FFX식), compact 할당으로 512MiB 회피, uint InterlockedMin 인코딩 정합, 패스 의존성(mark→allocate→scatter→finalize) 정확, LegacyEikonal 비교 유지.

**Med**
- `MaxScatterBricks` cap은 **compact 선택의 산물인 새 cap** (`64^3=262144` 중 65536=25%). FFX도 동일한 cap(브릭 풀 = `Free Bricks`)이 있으며 멀티캐스케이드+free-list로 잘 안 걸리게 관리할 뿐. **단일 dense 캐스케이드 bring-up은 예산만 넉넉히**: 부족하면 131072(256MiB), 메모리 여유 시 262144(=dense 동등, cap 무력화, 512MiB)도 선택지. **점유 브릭 worst-case(표면적 기반) 추정 + 낮춰서 overflow 테스트** 필수.
- sample→brick 매핑이 pass5(mark)·pass8(scatter)에서 **2회** 계산됨. v1 허용(캐싱하면 메모리↑).

**Low**
- 24-bit 인코딩 이유 명시(상위 바이트 예약 여부). 거리 정밀도엔 24-bit 충분.
- Legacy reference 버퍼는 **`LegacyEikonal` 선택 시에만 lazy-alloc** 권장(scatter scratch와 동시 상주 회피).
- 10패스는 rebuild-only(캐시됨)라 steady-state 무영향.

## C++/RenderGraph
- SparseSdfGI 전용 dispatch **command signature** 추가, indirect args는 `D3D12_DISPATCH_ARGUMENTS` raw buffer.
- 신규 scatter PSO들은 `ExactSharedBorder` build path에만 연결.
- `ExactSharedBorder`의 기존 `CSEmitTriangleReferences`/`CSSolveBrickReferences` exact variant 의존 제거.
- 루트 시그니처: 이미 b1 root CBV 전환 완료라 신규 bindless 여유 있음(현 `2+2+13` DWORD).

## 후속 단계 의존성 (로드맵)

권장 체인: **scatter (단일, build 품질) → sparse free-list 브릭 풀 → 멀티 캐스케이드**.

### 왜 scatter 먼저
- scatter = build 방식, 멀티캐스케이드 = 공간 구조 → 직교. scatter를 **단일 캐스케이드에서 검증**(큰-삼각형 홀/인플레이션/seam) 후 캐스케이드마다 반복하는 게 자연스러움. 반대로 하면 build를 다시 갈아엎게 됨.
- 멀티캐스케이드가 **scatter의 `MaxScatterBricks` cap 압박을 완화**(먼 지오메트리 = 거친 캐스케이드, 브릭 적게).

### 중간 단계: sparse 브릭 할당(free-list)
- FFX는 **캐스케이드 공유 브릭 풀 + free-list**(`Free Bricks`)를 사용 — 멀티캐스케이드의 사실상 전제.
- scatter의 **compact 할당이 이미 sparse로 가는 첫걸음** — free-list(회수)만 더하면 됨.

### 멀티 캐스케이드는 scatter보다 큼 (3축)
1. **Build**: scatter를 캐스케이드마다 호출 (scatter가 잘 돼 있으면 상대적으로 쉬움).
2. **Trace**: 현 hierarchical DDA는 **단일 캐스케이드 전용**. 멀티는 **coarse→fine 캐스케이드 순회 + start/end cascade 선택**(FFX `RayDesc.start/end_cascade_id`) 필요 → trace 계층을 per-cascade로 확장.
3. **클립맵 관리**: 카메라 추종 **scroll/wrap(toroidal 주소)**, 캐스케이드 경계 shared-border 정합, 캐스케이드 간 블렌딩.

### scatter 단계에서 미리 챙길 것 (멀티캐스케이드 대비)
- 단일 캐스케이드 하드코딩 금지: cascade bounds(`CascadeMin`/`VoxelSize`/extent)로 파라미터화(이미 constants).
- 브릭 주소를 **cascade-relative**로 (나중에 wrap 가능성).
- scratch/budget을 **per-cascade로 일반화 가능한 형태**로.

이러면 멀티캐스케이드가 **재작성이 아니라 추가**가 된다.

## 권장 Phase 분할

- **P1**: scatter 골격 (prepare/scan/args/mark/allocate/clear/scatter/finalize) — 단일 캐스케이드, 큰-삼각형 홀 제거 검증.
- **P2**: shared-border per-brick-local 열거(H1) + 밴드 두께(H2) 정합 — mode 5 seam / 구멍 검증.
- **P3**: legacy 경로/메모리 정리 (lazy-alloc), 예산 튜닝.
- **(후속)** sparse free-list → 멀티 캐스케이드.

## 테스트 / 검증

- **Compile**: 신규 shader entries `dxc`, Debug x64 MSBuild.
- **시나리오**: 큰 단일 바닥/벽 삼각형, 길고 얇은 삼각형, cascade 경계 삼각형.
- **디버그**: Exact Shared Border에서 **Shared Sample Mismatch(mode 5) seam**, Hit UVW, Brick Local Gradient, Step Count.
- **Acceptance**:
  - `MAX_TRIANGLE_BRICK_REFERENCES=4096` cap에 의한 **삼각형 drop 없음** (원래 동기).
  - scatter brick overflow 0 예산에서 **큰 삼각형 홀 없음**.
  - **seam 회귀 없음** (mode 5, gather 대비 동등 이하).
  - 추가 scratch가 `MaxScatterBricks` 예산에 비례, **dense 512MiB scratch 안 만듦**.
  - invalid bindless index / root signature 실패 / device removed 없음.
  - `MaxScatterBricks` 낮춰 overflow counter + invalid brick 동작 확인.

## 가정 / 범위 외
- v1은 single cascade `64^3` 유지.
- FFX SDK 코드/셰이더/상수는 복사하지 않고 **구조만 참고**.
- `MaxScatterBricks=65536`(128 MiB)는 품질 우선 bring-up 기본값.
- `LegacyEikonal`은 제거하지 않음. 제거 대상은 `ExactSharedBorder`의 gather solve뿐.

## 관련 문서
- [SparseSdfGI-ExactSharedBorderSDF-v1.md](SparseSdfGI-ExactSharedBorderSDF-v1.md) — exact shared-border 좌표계/빌드(현 gather).
- [SparseSdfGI-HierarchicalTrace-v1.md](SparseSdfGI-HierarchicalTrace-v1.md) — 계층 trace(scatter가 만든 SDF/metadata를 그대로 사용).
- [SparseSdfGI-v3.md](SparseSdfGI-v3.md) — gather(분할 참조 바이닝) 도입 히스토리(이 작업이 되돌리는 대상).
