## Working agreements
- When editing or creating files, preserve Windows line endings (CRLF, \r\n).
- Do not change line endings unless explicitly asked.
- If a file already uses CRLF, keep it CRLF.

## Version control workflow
- Stage the changes with `git add` (or an appropriate equivalent).
- Create a Git commit with a clear, descriptive message (include scope + summary; mention key files or behavior changes when helpful).

## Dead code / defensive checks
- Do not repeat null or availability guards in downstream helpers when the caller already established the same invariant for that execution slice.
- In render graph code, if pass setup computes an enable flag from required state such as root signatures, pipelines, or descriptors and the execute step forwards that flag, keep the runtime helper check at that flag instead of re-checking each input again.
- Keep a local guard only when the guarded state can still legitimately change between setup and use, and the code can name that invalidation path concretely.
- For persistent renderer inputs created during initialization or persistent descriptor creation, cache validation state at creation/refresh time instead of recomputing the full validation every frame.
- Per-frame validation should be limited to transient RenderGraph resources, frame-indexed bindless indices, resize/recreated resources, or inputs with a concrete invalidation path.

## Building the project

MSBuild is not on PATH. Use the full path:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "e:\UncleRenderer\UncleRenderer.vcxproj" `
    /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal `
    2>&1 | Select-String -Pattern "error C|error LNK" | Select-Object -First 30
```

- Configuration: `Debug` or `Release`
- Platform: `x64`
- `/m` enables parallel build
- `/v:minimal` suppresses noise; pipe through `Select-String "error C|error LNK"` to see only errors

## GPU resource usage

Prefer `FBindlessTexture` / `FBindlessBuffer` over raw `ID3D12Resource*` or `ComPtr<ID3D12Resource>` for GPU resources. These wrappers carry the resource, its `FRGTextureDesc`/`FRGBufferDesc`, current state, and bindless SRV/UAV indices together — keeping everything needed for RenderGraph import and bindless access in one place.

For resource creation, use the helpers in `Source/Render/GpuResource.h` instead of calling D3D12 APIs directly:

| Task | Helper |
|------|--------|
| Texture2D (default heap) | `CreateBindlessTexture(Device, Name, Desc, Flags, InitialState, OutTexture, bSrv, bUav, ClearValue)` |
| Buffer (default heap) | `CreateBindlessBuffer(Device, Name, Desc, InitialState, OutBuffer, bSrv, bUav)` |
| Upload buffer (with initial data) | `CreateUploadBuffer(Device, Name, Size, Buffer, SrcData)` |
| Persistently mapped upload buffer | `CreateMappedUploadBuffer(Device, Name, Size, Buffer)` |
| Mapped bindless buffer | `CreateMappedBindlessBuffer(Device, Name, Desc, OutBuffer, OutMappedData)` |
| `D3D12_HEAP_PROPERTIES` | `CreateHeapProperties(HeapType)` |
| `D3D12_RESOURCE_DESC` (buffer) | `CreateBufferResourceDesc(Size, Flags)` |
| `D3D12_RESOURCE_DESC` (texture2D) | `CreateTexture2DResourceDesc(Desc, Flags)` |
| Structured buffer desc | `CreateStructuredBufferDesc<T>(NumElements, ...)` |
| RW structured buffer desc | `CreateRWStructuredBufferDesc<T>(NumElements, ...)` |
| Register SRV into bindless heap | `CreateBindlessTextureSrv(Device, Texture)` / `CreateBindlessBufferSrv(Device, Buffer)` |
| Register UAV into bindless heap | `CreateBindlessTextureUav(Device, Texture)` / `CreateBindlessBufferUav(Device, Buffer)` |
| Import into RenderGraph | `ImportBindlessTexture(Graph, Name, Texture)` / `ImportBindlessBuffer(Graph, Name, Buffer)` |

Use `WriteOrCreateBindlessTextureSrv` / `WriteOrCreateBindlessTextureUav` when re-registering a resized or recreated resource that already has a slot in the bindless heap.

## Renderer variable ownership

Before adding a new variable to `Renderer.h` or `FRenderer`, check whether it belongs to an existing submodule instead:

- If the variable is only read/written by one submodule (e.g. `FGtao`, `FGpuDrivenCulling`, `FHzb`, `FSsr`, `FTaa`, `FDeferredBasePass`, etc.), add it to that submodule and expose it via a getter or `ApplyConfig`.
- If the variable is shared across multiple submodules or is fundamental to the base renderer lifecycle, it may belong on `FRenderer` or `FDeferredRenderer`.
- When in doubt, prefer the submodule — keeping config co-located with the logic that uses it avoids `FRenderer` becoming a god object.

### Clean code
- Before adding a local lambda or member function, verify whether a similar function already exists in the common headers. (GpuResource.h, MathTypes.h ..)
- Before writing new shader utility code, check `Shaders/Common.hlsli` and `Shaders/PBRCommon.hlsl` and `Shaders/CommonSH.hlsli` for an existing implementation of the same functionality. Do not duplicate helpers that are already provided there.
- Wrap any per-pixel/per-thread `ResourceDescriptorHeap[...]` index (e.g. from `sceneData`/material) in `NonUniformResourceIndex`, or AMD reads the wrong lane's descriptor.
- Never add or reorder `float`, `float2`, `float3`, or matrix fields in a shared C++/HLSL constants block without verifying HLSL 16-byte cbuffer packing and adding/updating `offsetof` static asserts.
- Keep code dry.
- Keep comments minimal. Prefer clear naming and structure over explanatory comments, and avoid multi-line comments unless they document non-obvious constraints or design decisions.
- RenderGraph pass executors run deferred: always capture local variables (handles, flags) by value, never by reference ([&]), to avoid dangling pointers.

## Documentation
- Add doc/FeatureName.md for all significant features; see doc/ClusterDAG-Streaming-v1.md for reference.
