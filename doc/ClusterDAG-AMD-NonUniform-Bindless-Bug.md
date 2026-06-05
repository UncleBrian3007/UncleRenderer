# ClusterDAG AMD Non-Uniform Bindless Descriptor 버그

## 요약

겹쳐 있는 ClusterDAG 오브젝트를 fullscreen visibility 버퍼 패스로 resolve할 때, AMD GPU에서 **엉뚱한 오브젝트의 geometry를 집어오는** 문제가 있었다. 증상은 뒤쪽 구가 앞쪽(더 가까운) 구를 뚫고 앞으로 새어나오는 것처럼 보였고, 특히 거리가 멀어질수록 두드러졌다.

근본 원인은 per-pixel `ClusterDagResolveSceneData`와 material 상태에서 가져온 bindless descriptor 인덱스에 **`NonUniformResourceIndex`가 빠진 것**이다. fullscreen resolve 패스에서는 같은 wave 안의 이웃 lane들이 서로 다른 `visibleEntry` / `drawData` / `sceneData` 레코드를 resolve할 수 있다. 따라서 이 descriptor 인덱스들은 **non-uniform**이며, 디버깅 시 한 픽셀만 보면 descriptor 인덱스가 안정적인 숫자로 보여도 실제로는 non-uniform이다.

## 증상

- 겹쳐 있는 ClusterDAG 구에서 **뒤쪽 오브젝트의 geometry가 앞쪽 오브젝트보다 앞에** 나타났다.
- 이 아티팩트는 **AMD에서만** 보였고, 테스트한 비-AMD 경로에서는 재현되지 않았다.
- 오브젝트가 카메라에서 **멀수록 더 뚜렷**해졌다.
- PIX로 보면 나쁜 픽셀이 `sceneData.ExtraBindlessIndices.y == 173`이라고 보고하는데, `LoadClusterDagTriangleIndices`가 반환하는 인덱스는 **다른 descriptor 슬롯(예: 슬롯 186)** 의 데이터와 일치했다.

PIX 관찰 예시:

```text
visibleEntry.DrawDataIndex = 12
drawData.StartIndex        = 4608
primitiveId                = 84
baseIndex                  = 4860
sceneData.ExtraBindlessIndices.y = 173

4860..4862 위치에서 기대되는 index buffer 값:
  921, 499, 500

실제 나쁜 픽셀에서 관찰된 값:
  41, 47, 38
```

## 배제한 가짜 단서들

### Reverse-Z depth 패킹

visibility 버퍼는 reverse-Z depth를 상위 32비트에 패킹한다:

```hlsl
const uint depthInt = asuint(saturate(Input.Position.z));
const uint64_t packedPixel = (uint64_t(depthInt) << 32u) | uint64_t(pixelValue);
InterlockedMax(Visibility64[uint2(Input.Position.xy)], packedPixel, previousValue);
```

`[0, 1]` 범위의 유한한 양수 depth 값에 대해 `asuint`는 순서를 보존하므로, reverse-Z + `InterlockedMax` 조합은 올바르다.

### 0 근처 / subnormal depth 붕괴

디버그 resolve 패스에서 저장된 depth 비트를 zero / subnormal / normal로 분류해봤다. 문제 픽셀들은 normal 범위에 머물렀으므로, 먼 거리의 depth 값이 0으로 붕괴해서 생긴 아티팩트는 아니었다.

### 64비트 typed UAV atomic max

같은 visibility 픽셀에 고정된 low/high 64비트 값을 `InterlockedMax`로 순서를 바꿔가며 써보는 격리 테스트를 했다. AMD는 어디서나 high 값으로 정확히 resolve했으므로, 64비트 atomic 자체는 직접적 원인이 아니었다.

## 근본 원인

`ClusterDagResolvePS`는 fullscreen 패스다. 각 픽셀은 패킹된 visibility 버퍼를 읽고 `visibleEntryIndex`를 디코드한 뒤 다음 체인을 따라간다:

```text
Visibility64 pixel
  -> visibleEntryIndex
  -> VisibleEntries[visibleEntryIndex]
  -> visibleEntry.DrawDataIndex
  -> DrawDatas[DrawDataIndex]
  -> drawData.DrawSectionIndex
  -> SceneDatas[DrawSectionIndex]
  -> sceneData.ExtraBindlessIndices / VertexBufferBindlessIndices / MaterialTextureIndices
```

같은 wave 안의 서로 다른 픽셀이 서로 다른 entry/scene data를 resolve할 수 있으므로, `sceneData`에서 로드한 descriptor 인덱스는 non-uniform이다. 다음과 같은 접근은 안전하지 않았다:

```hlsl
StructuredBuffer<uint> IndexBuffer = ResourceDescriptorHeap[sceneData.ExtraBindlessIndices.y];
```

올바른 형태는 다음과 같다:

```hlsl
StructuredBuffer<uint> IndexBuffer =
    ResourceDescriptorHeap[NonUniformResourceIndex(sceneData.ExtraBindlessIndices.y)];
```

이 annotation이 없으면 AMD는 wave 안 **다른 lane이 선택한 descriptor**를 사용할 수 있고, 그 결과 슬롯 173이 슬롯 186의 데이터를 읽는 것처럼 보이게 된다.

## 수정

per-pixel scene data 또는 material 상태에서 오는 ClusterDAG resolve descriptor 인덱스에 `NonUniformResourceIndex`를 적용했다.

핵심 파일:

- `Shaders/ClusterDag/ClusterDagGeometryFetch.hlsl`
  - ClusterDAG 런타임 index buffer
  - `sceneData`에서 로드하는 position, normal, UV, tangent, color 버퍼
- `Shaders/ClusterDagResolve.hlsl`
  - resolve된 `sceneData`에서 로드하는 material 텍스처 및 ClusterDAG 디버그 컬러 버퍼
- `Shaders/ClusterDag/RasterizeClusterSW.hlsl`
  - `sceneData`에서 로드하는 alpha-mask albedo 텍스처

## PIX 이름 정리

원본 primitive index buffer와 ClusterDAG 런타임 packed index buffer가 둘 다 `PrimitiveIndexBuffer`라는 이름이라 PIX 캡처에서 헷갈렸다.

ClusterDAG 런타임 packed index buffer는 이제 명시적 리소스 이름으로 생성한다:

```text
ClusterDagRuntimeIndexBuffer
```

이렇게 하면 PIX에서 원본 메시 index buffer와 ClusterDAG 런타임 index buffer를 쉽게 구분할 수 있다.

## 검증

1. AMD에서 겹친 ClusterDAG 구 두 개로 아티팩트를 재현했다.
2. 패킹된 depth가 zero/subnormal이 아니라 normal임을 확인했다.
3. `RWTexture2D<uint64_t>` + `InterlockedMax`가 더 큰 고정값을 올바르게 선택함을 확인했다.
4. PIX에서 한 descriptor 인덱스가 다른 descriptor 슬롯의 데이터를 읽는 것처럼 보이는 descriptor-수준 불일치를 확인했다.
5. non-uniform bindless 접근에 `NonUniformResourceIndex`를 적용했다.
6. 겹친 구를 재테스트하여 아티팩트가 해결됨을 확인했다.

## 회귀 방지 체크리스트

ClusterDAG fullscreen resolve, software raster, 기타 per-pixel bindless 읽기를 추가할 때:

- descriptor 인덱스가 **per-pixel 데이터**에서 오면 `NonUniformResourceIndex`로 감싼다.
- descriptor 인덱스가 **패스 전체 상수**이면 `NonUniformResourceIndex`는 필요 없다.
- fullscreen resolve 경로에서는 `visibleEntry`, `drawData`, `sceneData`, material 텍스처 인덱스, geometry 버퍼 인덱스가 lane마다 다를 수 있다고 가정한다.
- ClusterDAG 전용 GPU 리소스에는 PIX 캡처에서 런타임 경로를 명확히 식별할 수 있도록 구분되는 이름을 붙인다.
