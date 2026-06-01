# Virtual Shadow Map v1 구현 계획

## 요약

이 문서는 Unreal Engine 스타일 Virtual Shadow Map(VSM) 경로를 UncleRenderer에 추가하기 위한 초기 구현 계획을 정리합니다.

v1 목표는 현재 단일 방향광에 대해 동작하는 디퍼드 렌더러 구현입니다. 완전 GPU 구동 구현으로 가기 전에 VSM 페이지 마킹, 페이지 테이블 조회, 물리 아틀라스 렌더링, 라이팅 통합을 검증할 수 있도록 보수적인 CPU 보조 페이지 렌더링 경로에서 시작합니다.

기존 섀도우 맵 경로는 폴백 및 비교 모드로 유지합니다. 포워드 렌더링은 v1 범위에 포함되지 않습니다.

## UE_Release 비교

로컬 `E:\UE_Release` 구현은 의도한 장기 형태를 보여줍니다.

- UE는 `128x128` 페이지와 `16k x 16k` 가상 주소 공간(`VirtualShadowMapDefinitions.h`)을 사용합니다.
- 방향광은 클립맵(`VirtualShadowMapClipmap.h`)을 사용합니다.
- VSM은 디퍼드 렌더러 지향이며, UE는 포워드 셰이딩에서 VSM을 명시적으로 거부합니다.
- UE는 화면에 보이는 리시버에서 필요한 페이지를 마킹한 뒤, 요청/캐시된 물리 페이지만 렌더링합니다.
- UE는 v1에서 구현할 범위보다 훨씬 넓은 시스템을 갖습니다: GPU 페이지 관리, 캐시 무효화, 물리 페이지 메타데이터, 리시버 마스크, 코스 페이지, HZB 통합, Nanite/비-Nanite 드로우 커맨드 빌드, SMRT 필터링.

UncleRenderer v1은 UE의 핵심 개념을 맞추되 실행은 단순화해야 합니다.

- 가능한 범위에서 UE 정렬 상수를 유지: `PageSize=128`, 최대 `16k` 가상 해상도, 방향광 클립맵.
- UncleRenderer의 `FBindlessTexture`, `FBindlessBuffer` 리소스 소유 패턴 사용.
- UE의 GPU 페이지 할당/간접 페이지 렌더링 대신 1차 검증은 CPU 페이지 루프 사용.
- 페이지 캐시가 워밍업되는 동안 치명적인 검은 그림자 아티팩트를 피하기 위해 v1에서는 미싱 페이지를 조명됨(lit)으로 처리.

## 범위

v1 포함 항목:

- 디퍼드 렌더러만.
- 단일 방향광.
- 클립맵 기반 가상 그림자 투영.
- 카메라 가시 깊이로부터의 GPU 페이지 마킹.
- CPU 리드백 및 CPU 페이지 테이블/캐시 관리.
- 가능한 경우 기존 섀도우 드로우 경로를 재사용한 물리 깊이 아틀라스 렌더링.
- 활성화 시 VSM 기반 디퍼드 직접광 샘플링.
- VSM 비활성 또는 미사용 가능 시 기존 `ShadowMap` 폴백.
- 요청/할당/렌더링/미싱 페이지에 대한 기본 디버그 카운터 및 로깅.

v1 제외 항목:

- 포워드 렌더러 지원.
- 스팟/포인트/로컬 광원.
- UE 스타일 GPU 페이지 관리.
- Nanite/ClusterDAG 전용 VSM 래스터라이제이션.
- 페이지별 GPU 간접 드로우 빌드.
- 코스 페이지.
- 리시버 마스크.
- SMRT 소프트 섀도우.
- 정적/동적 분리 페이지 캐시 무효화.
- 페이지별 HZB 및 VSM 캐시 HZB.

## 렌더러 통합

`Source/Render/Deferred` 아래에 `FVirtualShadowMap`이라는 디퍼드 서브모듈을 추가합니다.

이 서브모듈은 VSM 전용 상태와 리소스를 모두 소유하며, 관련 없는 필드로 `FRenderer` 또는 `FDeferredRenderer`를 확장하지 않습니다. `FDeferredRenderer`는 `std::unique_ptr<FVirtualShadowMap>`만 보유하고, 다른 패스에서 필요할 때만 좁은 접근자(accessor)를 노출해야 합니다.

예상 책임:

- VSM 설정 적용.
- 파이프라인 초기화.
- 지속 리소스 초기화.
- 지속 리소스를 `FDeferredPassContext::Resources`로 import.
- 프레임 상태 준비.
- 현재 카메라/깊이에 필요한 페이지 마킹.
- CPU에서 페이지 요청 해석.
- 페이지 테이블 변경 업로드.
- dirty/할당 페이지를 물리 아틀라스에 렌더링.
- 라이팅용 바인드리스 인덱스/상수 제공.

VSM 활성 시 프레임 순서:

1. 기존 컬링 및 GBuffer/깊이 생성.
2. VSM 페이지 마크 패스가 깊이를 읽고 요청된 가상 페이지를 기록.
3. CPU가 이전에 준비된 리드백 버퍼의 페이지 요청을 소비.
4. CPU가 페이지 테이블과 dirty 페이지 목록을 갱신.
5. VSM이 dirty/할당된 물리 페이지를 렌더링.
6. 디퍼드 직접광이 VSM을 샘플링.

현재의 early shadow 패스는 레거시 경로를 위해 유지되어야 합니다. VSM이 먼저 전체 레거시 섀도우 맵을 렌더링해야만 동작하도록 의존하면 안 됩니다.

## 설정

다음 기본값을 `FRendererConfig`, `RendererConfigLoader`에 추가합니다.

```cpp
bool bEnableVirtualShadowMap = false;
uint32_t VirtualShadowMapPageSize = 128;
uint32_t VirtualShadowMapPhysicalAtlasSize = 4096;
uint32_t VirtualShadowMapClipmapCount = 6;
uint32_t VirtualShadowMapMaxRenderedPagesPerFrame = 512;
```

페이지 크기는 v1에서 셰이더 코드 상수 `128`로 고정 취급해도 됩니다. 설정 값은 의도를 문서화하고 검증/로깅을 가능하게 하기 위한 목적이 큽니다.

ImGui 컨트롤을 추가한다면 기존 Shadows 컨트롤 근처에 배치합니다.

- `Virtual Shadow Map`
- `VSM Clipmaps`
- `VSM Max Pages/Frame`
- 읽기 전용 통계: requested / resident / rendered / missing

## 리소스 모델

모든 지속 VSM GPU 리소스에 `FBindlessTexture` / `FBindlessBuffer`를 사용합니다.

핵심 리소스:

- 물리 깊이 아틀라스: `4096x4096`, 셰이더에서 읽을 수 있는 `R32_FLOAT` SRV를 가진 `D32_FLOAT` 깊이 리소스.
- 페이지 테이블 버퍼 또는 텍스처: 가상 페이지 키를 물리 페이지 좌표와 상태 비트에 매핑.
- 페이지 요청 버퍼: GPU가 요청된 가상 페이지를 기록.
- 페이지 요청 카운터/헤더.
- 요청용 프레임별 리드백 버퍼.
- 페이지 테이블 업데이트용 매핑 업로드 버퍼.

권장 v1 CPU 구조체:

```cpp
struct FVirtualShadowPageKey
{
    uint32_t ClipmapIndex;
    uint32_t PageX;
    uint32_t PageY;
};

struct FVirtualShadowPageEntry
{
    uint32_t PhysicalPageIndex;
    uint32_t Flags;
    uint32_t LastRequestedFrame;
    uint32_t LastRenderedFrame;
};
```

Flags는 최소 다음을 포함해야 합니다.

- resident
- dirty
- requested this frame

물리 페이지 인덱스는 아틀라스 좌표로 다음과 같이 매핑합니다.

```cpp
PhysicalX = PhysicalPageIndex % PhysicalPagesPerRow;
PhysicalY = PhysicalPageIndex / PhysicalPagesPerRow;
```

아틀라스 `4096`, 페이지 크기 `128`이면 풀 크기는 `32 x 32 = 1024` 물리 페이지입니다.

## 페이지 마킹

페이지 마크 패스는 현재 깊이 버퍼를 읽고, 다른 디퍼드 패스와 동일한 카메라 행렬로 월드 위치를 복원하는 컴퓨트 셰이더여야 합니다.

각 유효 깊이 샘플에 대해:

1. 월드 위치를 복원.
2. 카메라/클립맵 중심 거리 기반으로 방향광 클립맵 선택.
3. 월드 위치를 라이트 클립맵 공간으로 변환.
4. 투영된 섀도우 UV를 가상 텍셀 좌표로 변환.
5. 가상 텍셀 좌표를 가상 페이지 좌표로 변환.
6. 요청 페이지 키를 append 하거나 원자적으로 마킹.

v1에서는 샘플된 화면 픽셀당 1개 요청이면 충분합니다. 요청 압력이 너무 높으면 이후 stride 옵션을 추가할 수 있습니다.

셰이더는 무효 좌표를 clamp하고 배경 깊이는 무시해야 합니다.

## CPU 페이지 관리

현재 프레임에서 즉시 스톨하는 대신, 프레임 지연이 있는 리드백을 사용합니다.

프레임별 처리:

- 가장 오래된 완료 요청 버퍼 소비.
- 페이지 키 중복 제거.
- 요청된 resident 페이지를 최근 사용으로 표시.
- 미싱 요청 페이지에 대해 여유 물리 페이지 할당.
- 풀이 가득 찼다면 최소 최근 사용(LRU) 페이지 축출.
- 새로 할당되거나 재사용된 페이지를 dirty로 표시.
- 변경된 페이지 테이블 엔트리 업로드.
- 물리 페이지 렌더링 수를 `VirtualShadowMapMaxRenderedPagesPerFrame`으로 제한.

라이팅에서 미싱 페이지는 v1 기준 조명됨(`shadow=1.0`)을 반환해야 합니다.

## 물리 페이지 렌더링

가능하면 기존 섀도우 맵 버텍스 경로를 재사용합니다.

해당 프레임에서 선택된 각 dirty 페이지에 대해:

- 해당 페이지의 `128x128` 아틀라스 사각형으로 viewport/scissor 설정.
- 가능하면 해당 페이지 사각형만 클리어. D3D12에서 부분 깊이 클리어가 까다롭다면 v1 검증 단계에서는 프레임 시작 시 아틀라스 전체 클리어를 허용하되, 목표 정상 상태 동작은 페이지별 클리어.
- 클립맵 페이지용 페이지 전용 라이트 뷰/프로젝션 또는 crop 변환 구성.
- `FDeferredBasePass::AddShadowPass`의 기존 모델 반복, 알파 블렌드 스킵, 스키닝 지원, 섀도우 PSO 선택 재사용.
- 가능하면 페이지 투영에 대해 모델 프러스텀 컬링 수행.

이 CPU 페이지 루프는 UE 구현보다 느릴 것으로 예상됩니다. 하지만 v1에서는 데이터 모델과 라이팅 경로를 검증한다는 목적에 부합합니다.

## 라이팅

디퍼드 직접광은 섀도우 소스를 다음처럼 선택해야 합니다.

- VSM 활성 + 리소스 유효: VSM 샘플링.
- 그 외: 기존 레거시 섀도우 맵 샘플링.
- 레이 트레이스 섀도우 마스크 동작은 변경 없이 유지되며, 활성 시 래스터 섀도우를 계속 오버라이드해야 함.

셰이더 변경:

- 페이지 테이블 조회 및 아틀라스 샘플링을 위한 VSM 헬퍼 include 추가.
- 라이팅 바인드리스 상수에 VSM 아틀라스/페이지 테이블 인덱스와 enable 플래그 확장.
- 월드 위치를 방향광 클립맵 공간으로 변환.
- 페이지 테이블 조회.
- 페이지가 resident면 물리 아틀라스 깊이를 샘플링해 비교.
- 페이지가 미싱이면 lit 반환.

초기 필터는 아틀라스 텍셀에 대한 작은 고정 PCF로 충분합니다. 더 고급 UE 스타일 SMRT 필터링은 나중으로 미룹니다.

## 문서화

구현 결정이 바뀌면 이 문서를 계속 업데이트하세요.

v1 구현이 적용되면, 다음을 설명하는 짧은 "Current Status" 섹션을 추가하세요.

- 구현된 패스
- 알려진 제한 사항
- 디버그 컨트롤
- 성능 관련 주의점

## 테스트 계획

빌드:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "e:\UncleRenderer\UncleRenderer.vcxproj" `
    /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal `
    2>&1 | Select-String -Pattern "error C|error LNK" | Select-Object -First 30
```

검증 시나리오:

- `bEnableVirtualShadowMap=false`: 기존 섀도우 맵 경로가 변경 없이 렌더링되어야 함.
- `bEnableVirtualShadowMap=true`: 방향광 그림자가 VSM을 통해 렌더링되어야 함.
- Sponza/기본 씬에서 카메라 이동: 페이지 요청 수와 resident 페이지 수가 변해야 함.
- 런타임/설정 리로드에서 VSM 비활성화: 렌더러가 레거시 섀도우 맵으로 폴백해야 함.
- 레이 트레이스 섀도우 활성화: 기존 섀도우 마스크 경로가 계속 최우선이어야 함.
- `VirtualShadowMapMaxRenderedPagesPerFrame`를 낮춰 스트레스 테스트: 미싱 페이지는 검정이 아니라 lit로 보여야 함.

저장소 위생:

- CRLF 줄바꿈 유지.
- 관련 없는 로컬 변경은 스테이징하지 않기.
- `git diff --check` 실행.
- `docs: add virtual shadow map v1 plan` 같은 범위 있는 메시지로 커밋.
