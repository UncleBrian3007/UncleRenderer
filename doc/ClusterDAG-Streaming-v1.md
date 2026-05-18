# ClusterDAG Streaming v1/v2 - 피드백 루프와 우선순위 스케줄러

## 문서 역할

이 문서는 ClusterDAG streaming의 초기 구조인 v1/v2를 설명한다. v1/v2는 GPU가 "필요하지만 아직 resident가 아닌 page"를 feedback buffer에 기록하고, CPU가 그 요청을 읽어 page table의 resident bit를 갱신하는 기반을 만들었다.

현재 구현은 v3 단계까지 진행되어 실제 `.vmesh` page directory, disk I/O, `PageDataBuffer` upload, shader-side paged fetch 일부가 들어가 있다. 따라서 이 문서는 최신 전체 동작 설명이 아니라 다음을 위한 기준 문서로 둔다.

- GPU feedback/readback 구조 이해
- pending page priority scheduler 이해
- v3에서 확장된 부분이 어디에서 출발했는지 확인

최신 실제 I/O와 page data 경로는 `ClusterDAG-Streaming-v3-Plan.md`를 기준으로 본다.

## v1/v2 범위

### v1

- GPU traversal 중 non-resident page를 발견하면 feedback UAV에 request를 기록한다.
- CPU는 frame fence signal 이후 feedback readback buffer를 map해서 request를 소비한다.
- page table CPU mirror를 갱신하고, 다음 frame에 GPU page table buffer로 업로드한다.
- page 0(root)은 시작부터 resident이고, non-root page는 요청 전까지 non-resident다.

### v2

- v1 feedback loop 위에 CPU-side priority scheduler를 추가했다.
- 같은 page의 중복 request는 `max(Priority)`로 병합한다.
- pending page가 꽉 차면 더 낮은 priority request는 drop하고, 더 높은 priority request는 기존 최저 priority pending page를 대체한다.
- 설치 순서는 priority 내림차순, 최초 요청 serial 오름차순이다.

### v1/v2에 없던 것

- 실제 disk I/O 없음
- `.vmesh` page directory 없음
- `PageDataBuffer`에 page payload upload 없음
- GPU upload fence 완료 후 resident 전환 없음
- shader가 physical page slot에서 payload를 직접 읽는 경로 없음
- eviction 없음

## Frame Flow

```text
GPU traversal
  - ClusterDAG traversal 중 non-resident page 감지
  - feedback UAV에 streaming request 기록
  - 해당 branch refinement 중단

GPU readback pass
  - feedback UAV 내용을 frame별 readback buffer로 copy

CPU OnFrameFenceSignaled
  - readback buffer map
  - request count와 request entries 소비
  - duplicate page request 병합
  - pending queue 갱신

다음 frame begin
  - pending page를 priority 순서로 resident 처리
  - page table upload dirty이면 PageTableBuffer 갱신
  - feedback buffer counter clear
```

v3에서는 위 flow의 "pending page를 resident 처리" 단계가 disk read, GPU upload, upload fence retire를 거친 뒤 resident로 바뀌도록 확장되었다.

## 주요 리소스

| Resource | v1/v2 역할 | v3 이후 상태 |
|----------|------------|--------------|
| `PageTableBuffer` | shader가 읽는 page table SRV | 계속 사용 |
| `PageTableUpload` | CPU mirror를 GPU로 복사하기 위한 mapped upload buffer | 계속 사용 |
| `FeedbackBuffers` | shader request 기록용 frame별 UAV | 계속 사용 |
| `FeedbackReadbackBuffers` | CPU request 소비용 frame별 readback buffer | 계속 사용 |
| `PageDataBuffer` | 생성만 되고 실제 payload는 없음 | v3.2부터 physical page slot pool |

## Page Table

```cpp
struct FClusterDagPageTableEntry
{
    uint32_t PhysicalPageIndex;
    uint32_t Flags;
    uint32_t LastUsedFrame;
    uint32_t Reserved;
};
```

v1/v2에서는 `PhysicalPageIndex`가 사실상 logical page index처럼 쓰였고 resident bit만 중요했다. v3.2부터는 `PhysicalPageIndex`가 `PageDataBuffer` 안의 physical slot index를 의미한다.

초기화 규칙:

```cpp
// Page 0(root): startup resident
Entry[0].PhysicalPageIndex = 0;
Entry[0].Flags |= GClusterDagPageResidentFlag;

// Non-root: request 전까지 non-resident
Entry[N].PhysicalPageIndex = 0xffffffffu;
Entry[N].Flags = 0;
```

## Streaming Request

```cpp
struct FClusterDagStreamingRequest
{
    uint32_t StreamingResourceId;
    uint32_t PageIndex;
    uint32_t Priority;
    uint32_t Flags;
};
```

request buffer의 첫 entry는 실제 request가 아니라 counter 용도로 사용한다. shader는 `InterlockedAdd`로 request index를 확보하고, capacity를 넘으면 overflow counter를 증가시킨다.

`Priority`는 현재 traversal에서 관측한 LOD error 기반 값이다. v2 scheduler는 같은 page의 request를 병합할 때 가장 큰 priority를 유지한다.

## Shader Gate

```hlsl
bool ShouldRefineClusterDagStreamingPage(...)
{
    if (!streamingEnabled)
        return true;

    const uint pageIndex = GetClusterDagGroupPageIndex(nextGroup);
    if (pageIndex == 0 || IsClusterDagPageResident(pageIndex, PageTable))
        return true;

    RequestClusterDagStreamingPage(...);
    return false;
}
```

이 함수가 v1/v2의 핵심이다. resident page는 계속 refine하고, non-resident page는 request만 남긴 뒤 현재 branch를 멈춘다. v3.3에서도 non-resident page에 대한 안전장치는 이 구조를 유지한다.

## Scheduler 동작

```text
QueueRequestedPage(page, priority)
  - 이미 resident이면 무시
  - 이미 pending이면 priority = max(old, new)
  - pending cap 여유가 있으면 추가
  - cap이 꽉 찼고 새 request가 최저 priority보다 높으면 교체
  - 아니면 drop

InstallPendingPages()
  - priority desc
  - first request serial asc
  - MaxPageInstallsPerFrame 만큼 처리
```

v3에서는 `InstallPendingPages()`의 의미가 바뀌었다. 즉시 resident로 바꾸지 않고 다음 pipeline으로 나뉜다.

```text
IssuePageReads()
PollIoCompletions()
IssuePageUploads()
RetireCompletedUploads()
```

## Config와 UI

v1/v2 기준 config:

```ini
[ClusterDAG]
bEnableClusterDAGStreaming=true
ClusterDAGStreamingPoolMB=256
ClusterDAGStreamingRequestBufferCapacity=65536
ClusterDAGStreamingMaxPendingPages=64
ClusterDAGStreamingMaxPageInstallsPerFrame=16
```

현재 UI에는 v3에서 추가된 다음 항목도 함께 표시된다.

- `Cluster DAG Max IO In Flight`
- `Cluster DAG Max Upload MB/Frame`
- `Cluster DAG Page Slot KB` read-only 표시
- request, I/O, upload, slot pool stats

`ClusterDAGStreamingPageSlotBytes` config 필드는 legacy로 남아 있지만 runtime slot size는 `.vmesh`와 맞춘 `GClusterDAGVmeshStreamingPageSlotBytes`를 사용한다.

## Debug Stats

v1/v2에서 중요한 GPU debug stat:

- `STREQ`: shader가 기록한 streaming request 수
- `STFALL`: non-resident page 때문에 refinement를 멈춘 수
- `STDROP`: feedback request buffer overflow로 drop된 수

v3에서는 여기에 CPU/ImGui 쪽 I/O, upload, slot pool counter가 추가되었다.

## v3와의 관계

v3는 v1/v2를 대체하기보다 확장한다.

| 단계 | v1/v2 | v3 |
|------|-------|----|
| request 생성 | GPU feedback UAV | 동일 |
| request 소비 | frame fence 이후 CPU readback | 동일 |
| 중복 request 병합 | priority scheduler | 동일 |
| page 설치 | resident bit 즉시 set | disk read + GPU upload + fence retire 후 resident |
| payload 저장 | 없음 | `.vmesh` page directory |
| GPU payload | 없음 | `PageDataBuffer` physical slot |
| shader fetch | global scene/model buffers | v3.3부터 group/child-ref는 paged fetch |

## 참고 파일

- `Source/Render/Deferred/ClusterDagStreamingManager.h/cpp`
- `Source/Render/Deferred/ClusterDagRuntime.cpp`
- `Shaders/ClusterDag/ClusterDagTraversalCommon.hlsl`
- `Shaders/ClusterDag/ClusterDagNodeCull.hlsl`
- `Source/Core/RendererConfig.h/cpp`
- `Source/Core/Application.cpp`
