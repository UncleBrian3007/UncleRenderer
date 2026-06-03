# SparseSdfGI Screen Probe Crash v1

이 문서는 SparseSdfGI screen probe와 diffuse GI denoiser를 함께 켰을 때
발생했던 D3D12 device removed crash의 조사 과정과 최종 수정 내용을 기록한다.

## 증상

Crash는 대체로 아래 설정에서 재현되었다.

```ini
EnableDiffuseGIDenoiser=true
SparseSdfGIUseScreenProbes=true
SparseSdfGIMultiBounce=false
SparseSdfGIProbeTemporalReuse=false
SparseSdfGIProbeSpawnJitter=false
SparseSdfGIProbeMotionReproject=false
SparseSdfGiProbeDebugMode=0
```

`SparseSdfGIProbeTileSize=16`, `SparseSdfGIProbeRaysPerProbe=4`처럼 probe
workload를 줄여도 crash는 사라지지 않았다.

대표적인 DRED breadcrumb는 screen probe 경로를 가리켰다.

```text
SparseSdfGI Probe Trace
SparseSdfGI Probe Interpolate
RestirGI Denoiser / Denoiser PreBlur
DRED PageFault VA=0x0 또는 낮은 GPU VA
GetDeviceRemovedReason = 0x887A0006
```

D3D12 debug layer를 켰을 때는 `DXGI_ERROR_DEVICE_HUNG`로 보고되었다.
Debug layer가 꺼져 있을 때는 보통 더 뒤쪽의 `IDXGISwapChain::Present` 또는
`ID3D12CommandQueue::Signal(Flush)`에서 device removed로 드러났다.

## 파악한 원인

Crash는 transient screen-probe header buffer를 shader에서 읽는 지점으로
좁혀졌다.

```hlsl
StructuredBuffer<FSparseSdfGIProbeHeader> probeHeaders =
    ResourceDescriptorHeap[ProbeHeaderSrvIndex];

const FSparseSdfGIProbeHeader header = probeHeaders[probeIndex];
```

중요한 관찰 결과는 다음과 같다.

- `ProbeHeader` buffer를 만들고 SRV/UAV descriptor를 등록하는 것만으로는
  crash가 발생하지 않았다.
- RenderGraph resource와 descriptor plumbing은 유지한 채 shader에서
  `probeHeaders[probeIndex]`를 읽지 않게 만들면 crash가 사라졌다.
- lane 0 하나에서만 header를 읽어도 crash가 다시 재현되었다.
- `ProbeHeader` SRV read 없이 SDF tracing, blue-noise sampling, probe write를
  복구했을 때는 안정적으로 동작했다.

따라서 주 원인은 ray count, SDF trace 비용, denoiser 비용이 아니라
transient `StructuredBuffer` SRV read 경로에 있는 것으로 판단했다. 가능한
분류는 descriptor lifetime 문제, resource aliasing, stale bindless index,
또는 probe header buffer 주변의 RenderGraph transition/import mismatch이다.

## 수정 방향

최종 방향은 screen probe pipeline에서 `ProbeHeader`를 별도 GPU buffer로
두지 않는 것이다. Screen probe header는 기존 depth와 GBuffer 데이터에서
저렴하게 재구성할 수 있으므로, 문제가 된 transient `StructuredBuffer` SRV
read를 완전히 피할 수 있다.

기존 구조는 다음과 같았다.

```text
SparseSdfGI Probe Spawn
  -> ProbeHeader UAV write

SparseSdfGI Probe Trace
  -> ProbeHeader SRV read

SparseSdfGI Probe Interpolate
  -> ProbeHeader SRV read
```

수정 후 구조는 다음과 같다.

```text
SparseSdfGI Probe Trace
  -> depth + GBufferA read
  -> probe header를 local로 재구성
  -> ProbeSH / ProbeVariance / ProbeHistory write

SparseSdfGI Probe Interpolate
  -> depth + GBufferA read
  -> neighbor probe header를 local로 재구성
  -> ProbeSH / ProbeVariance read
  -> full-res GI / denoiser input write
```

공통 shader helper는 다음 함수다.

```hlsl
FSparseSdfGIProbeHeader BuildScreenProbeHeaderFromDepth(
    uint2 probeCoord,
    Texture2D<float> depthTexture,
    Texture2D<float4> gbufferA)
```

## 코드 변경

먼저 crash 회피를 위해 shader의 실제 `ProbeHeader` read를 제거했다.

- Commit: `6fa6b6f SparseSdfGI: avoid probe header SRV reads`
- `CSTraceScreenProbes`는 depth/GBuffer에서 header를 재구성한다.
- `CSInterpolateScreenProbes`는 neighbor header를 depth/GBuffer에서 재구성한다.

이후 최종 정리로 사용하지 않는 header pass와 root constant를 제거했다.

- Commit: `d2ab528 SparseSdfGI: remove screen probe header pass`
- `SparseSdfGI Probe Spawn` pass 제거.
- `CSSpawnScreenProbes` shader compile 및 PSO 생성 제거.
- `ProbeSpawnPipeline` 제거.
- `ProbeHeader` transient buffer 생성 제거.
- Probe root constant에서 `ProbeHeaderSrvIndex` / `ProbeHeaderUavIndex` 제거.
- Header buffer에 대한 RenderGraph read/write 선언 제거.

Trace shader가 실제로 사용하는 입력을 RenderGraph가 추적할 수 있도록 trace
pass에는 depth와 GBufferA read 선언을 남겼다.

```cpp
Builder.ReadTexture(DepthHandle, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
Builder.ReadTexture(GBufferHandles[0], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
```

Root signature 크기를 늘리지 않기 위해 trace shader의 depth/GBuffer bindless
index는 기존 `FSparseSdfGIConstants` field를 재사용해 전달한다.

```cpp
Constants.ModelTriangleCount = DepthBindlessIndex;
Constants.ModelDrawIndexStart = GBufferABindlessIndex;
```

Shader 쪽에서는 alias helper로 의도를 명확히 한다.

```hlsl
uint GetTraceDepthTextureIndex() { return ModelTriangleCount; }
uint GetTraceGBufferATextureIndex() { return ModelDrawIndexStart; }
```

## 검증

최종 정리 후 다음을 확인했다.

- `dxc`로 `CSTraceScreenProbes` compile 통과.
- `dxc`로 `CSInterpolateScreenProbes` compile 통과.
- Debug x64 MSBuild가 프로젝트 error filter 기준으로 통과.
- 보고된 screen-probe + denoiser 설정에서 기존 device-removed crash가 더 이상
  재현되지 않았다.

## Tradeoff / 후속 작업

Header buffer를 제거하면서 기존 screen-probe spawn jitter와 velocity 기반
probe motion reprojection 경로도 함께 제거되었다. 그 결과는 다음과 같다.

- 현재 screen probe 구현에서 `SparseSdfGIProbeSpawnJitter`는 더 이상 동작하지
  않는다.
- 현재 screen probe 구현에서 `SparseSdfGIProbeMotionReproject`는 더 이상
  동작하지 않는다.
- `SparseSdfGIProbeTemporalReuse`는 여전히 이전 probe history와 blend하지만,
  header를 motion reprojection 없이 재구성하므로 카메라 이동 상황에서는 on/off
  차이가 약하거나 거의 보이지 않을 수 있다.

Denoiser를 끈 상태에서 screen-probe noise가 너무 크다면, 다음 방향은
transient header SRV read를 다시 도입하지 않고 temporal reuse를 강화하는 것이다.
가능한 선택지는 다음과 같다.

- Previous-probe mapping을 transient RenderGraph buffer가 아니라 명확한 lifetime을
  가진 persistent bindless buffer에 저장한다.
- `CSTraceScreenProbes`에서 velocity texture를 추가 입력으로 받아 motion-reprojected
  previous probe index를 직접 계산한다.
- 현재 depth/GBuffer 재구성과 `ProbeHistory`만 사용하는 저비용 temporal
  accumulation 경로를 추가한다.
