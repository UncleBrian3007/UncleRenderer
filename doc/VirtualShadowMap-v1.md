# Virtual Shadow Map v1 Implementation Plan

## Summary

This document captures the initial implementation plan for adding an Unreal Engine-style Virtual Shadow Map (VSM) path to UncleRenderer.

The v1 target is a working deferred-renderer implementation for the existing single directional light. It intentionally starts with a conservative CPU-assisted page rendering path so the renderer can validate VSM page marking, page table lookup, physical atlas rendering, and lighting integration before moving toward a fully GPU-driven implementation.

The existing shadow map path remains available as a fallback and comparison mode. Forward rendering is out of scope for v1.

## UE_Release Comparison

The local `E:\UE_Release` implementation shows the intended long-term shape:

- UE uses `128x128` pages and a `16k x 16k` virtual address space (`VirtualShadowMapDefinitions.h`).
- Directional lights use clipmaps (`VirtualShadowMapClipmap.h`).
- VSM is deferred-renderer-oriented; UE explicitly rejects VSM in forward shading.
- UE marks required pages from screen-visible receivers, then renders only requested/cached physical pages.
- UE has a much broader system than v1 will implement: GPU page management, cache invalidation, physical page metadata, receiver masks, coarse pages, HZB integration, Nanite/non-Nanite draw command building, and SMRT filtering.

UncleRenderer v1 should match UE's core concepts but simplify execution:

- Keep UE-aligned constants where practical: `PageSize=128`, virtual resolution up to `16k`, directional clipmaps.
- Use UncleRenderer's `FBindlessTexture` and `FBindlessBuffer` resource ownership patterns.
- Use a CPU page loop for first validation instead of UE's GPU page allocation and indirect page rendering.
- Treat missing pages as lit in v1 to avoid catastrophic black shadow artifacts while the page cache warms up.

## Scope

Included in v1:

- Deferred renderer only.
- One directional light.
- Clipmap-based virtual shadow projection.
- GPU page marking from camera-visible depth.
- CPU readback and CPU page table/cache management.
- Physical depth atlas rendering using the existing shadow draw path where possible.
- Deferred direct lighting sampling from VSM when enabled.
- Existing `ShadowMap` fallback when VSM is disabled or unavailable.
- Basic debug counters/logging for requested, allocated, rendered, and missing pages.

Excluded from v1:

- Forward renderer support.
- Spot/point/local lights.
- UE-style GPU page management.
- Nanite/ClusterDAG-specific VSM rasterization.
- GPU indirect per-page draw building.
- Coarse pages.
- Receiver masks.
- SMRT soft shadows.
- Static/dynamic separated page cache invalidation.
- Per-page HZB and VSM cache HZB.

## Renderer Integration

Add a deferred submodule named `FVirtualShadowMap` under `Source/Render/Deferred`.

The submodule owns all VSM-specific state and resources instead of expanding `FRenderer` or `FDeferredRenderer` with many unrelated fields. `FDeferredRenderer` should hold only a `std::unique_ptr<FVirtualShadowMap>` and expose narrow accessors only if another pass requires them.

Expected responsibilities:

- Apply VSM config.
- Initialize pipelines.
- Initialize persistent resources.
- Import persistent resources into `FDeferredPassContext::Resources`.
- Prepare frame state.
- Mark pages needed by the current camera/depth.
- Resolve page requests on CPU.
- Upload page table changes.
- Render dirty/allocated pages into the physical atlas.
- Provide bindless indices/constants for lighting.

Frame order when VSM is enabled:

1. Existing culling and GBuffer/depth generation.
2. VSM page mark pass reads depth and writes requested virtual pages.
3. CPU consumes page requests from the previous available readback buffer.
4. CPU updates page table and dirty page list.
5. VSM renders dirty/allocated physical pages.
6. Deferred direct lighting samples VSM.

The current early shadow pass should remain for the legacy path. VSM should not depend on rendering the full legacy shadow map first.

## Config

Add these defaults to `FRendererConfig` and `RendererConfigLoader`:

```cpp
bool bEnableVirtualShadowMap = false;
uint32_t VirtualShadowMapPageSize = 128;
uint32_t VirtualShadowMapPhysicalAtlasSize = 4096;
uint32_t VirtualShadowMapClipmapCount = 6;
uint32_t VirtualShadowMapMaxRenderedPagesPerFrame = 512;
```

The page size can be treated as fixed at `128` in shader code for v1. The config value exists mainly to document the intent and allow validation/logging.

If ImGui controls are added, place them near the existing Shadows controls:

- `Virtual Shadow Map`
- `VSM Clipmaps`
- `VSM Max Pages/Frame`
- read-only stats: requested / resident / rendered / missing

## Resource Model

Use `FBindlessTexture` / `FBindlessBuffer` for all persistent VSM GPU resources.

Core resources:

- Physical depth atlas: `4096x4096`, `D32_FLOAT` depth resource with shader-readable `R32_FLOAT` SRV.
- Page table buffer or texture: maps virtual page keys to physical page coordinates and status bits.
- Page request buffer: GPU writes requested virtual pages.
- Page request counter/header.
- Per-frame readback buffers for requests.
- Mapped upload buffer for page table updates.

Recommended v1 CPU structures:

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

Flags should include at minimum:

- resident
- dirty
- requested this frame

Physical page index maps to atlas coordinates:

```cpp
PhysicalX = PhysicalPageIndex % PhysicalPagesPerRow;
PhysicalY = PhysicalPageIndex / PhysicalPagesPerRow;
```

For a `4096` atlas and `128` page size, the pool contains `32 x 32 = 1024` physical pages.

## Page Marking

The page mark pass should be a compute shader that reads the current depth buffer and reconstructs world position using the same camera matrices used by other deferred passes.

For each valid depth sample:

1. Reconstruct world position.
2. Select a directional clipmap based on distance from the camera/clipmap center.
3. Transform world position into light clipmap space.
4. Convert projected shadow UV to virtual texel coordinates.
5. Convert virtual texel coordinates to virtual page coordinates.
6. Append or atomically mark the requested page key.

For v1, one request per sampled screen pixel is acceptable. A stride option can be added later if request pressure is too high.

The shader should clamp invalid coordinates and ignore background depth.

## CPU Page Management

Use readback with frame latency rather than stalling immediately on the current frame.

Per frame:

- Consume the oldest completed request buffer.
- Deduplicate page keys.
- Mark requested resident pages as recently used.
- Allocate free physical pages for missing requested pages.
- If the pool is full, evict least-recently-used pages.
- Mark newly allocated or evicted/reused pages dirty.
- Upload changed page table entries.
- Limit physical page rendering to `VirtualShadowMapMaxRenderedPagesPerFrame`.

Missing pages in lighting should return lit (`shadow=1.0`) for v1.

## Physical Page Rendering

Reuse the existing shadow map vertex path where possible.

For each dirty page selected this frame:

- Set viewport/scissor to that page's `128x128` atlas rectangle.
- Clear only that page rectangle when practical. If partial depth clear is awkward in D3D12, v1 may clear the whole atlas at frame start only while validating, but the desired steady-state behavior is per-page clear.
- Build a page-specific light view/projection or crop transform for the clipmap page.
- Reuse existing model iteration, alpha-blend skip, skinning support, and shadow PSO selection from `FDeferredBasePass::AddShadowPass`.
- Frustum-cull models against the page projection where possible.

This CPU page loop is expected to be slower than UE's implementation. It is acceptable for v1 because it validates the data model and lighting path.

## Lighting

Deferred direct lighting should choose the shadow source:

- VSM enabled and resources valid: sample VSM.
- Otherwise: sample the existing legacy shadow map.
- Ray traced shadow mask behavior should remain unchanged and continue to override raster shadows when enabled.

Shader changes:

- Add a VSM helper include for page table lookup and atlas sampling.
- Extend lighting bindless constants with VSM atlas/page table indices and enable flag.
- Convert world position to directional clipmap space.
- Look up the page table.
- If page is resident, sample physical atlas depth and compare.
- If page is missing, return lit.

The initial filter can be a small fixed PCF over atlas texels. More advanced UE-style SMRT filtering is deferred.

## Documentation

Keep this document updated as implementation decisions change.

If the v1 implementation lands, add a short "Current Status" section describing:

- implemented passes
- known limitations
- debug controls
- performance caveats

## Test Plan

Build:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "e:\UncleRenderer\UncleRenderer.vcxproj" `
    /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal `
    2>&1 | Select-String -Pattern "error C|error LNK" | Select-Object -First 30
```

Validation scenarios:

- `bEnableVirtualShadowMap=false`: existing shadow map path renders unchanged.
- `bEnableVirtualShadowMap=true`: directional shadows render through VSM.
- Move camera through Sponza/default scene: page requests and resident page count change.
- Disable VSM at runtime/config reload: renderer falls back to legacy shadow map.
- Enable ray traced shadows: existing shadow mask path remains authoritative.
- Stress with `VirtualShadowMapMaxRenderedPagesPerFrame` set low: missing pages should appear lit rather than black.

Repository hygiene:

- Preserve CRLF line endings.
- Do not stage unrelated local changes.
- Run `git diff --check`.
- Commit with a scoped message such as `docs: add virtual shadow map v1 plan`.
