# SparseSdfGI FFX-Style Multi-Cascade v1

## Summary

SparseSdfGI v1 multi-cascade follows the FFX Brixelizer resource shape:

- one shared `512^3` SDF atlas
- per-cascade `64^3` logical brick maps
- shared physical brick metadata/radiance/irradiance buffers
- fixed atlas partition per cascade instead of a free-list

Deferred follow-ups are shared brick free-list, toroidal clipmap scrolling, eikonal propagation, cascade merge, and per-cascade round-robin build budgets.

## Resource Layout

The SDF atlas is selected from `Auto`, `R8`, and `R16`.

- `Auto` prefers `DXGI_FORMAT_R8_UNORM` when Texture3D typed UAV support exists.
- `Auto` falls back to `DXGI_FORMAT_R16_UNORM`.
- `R32_FLOAT` is intentionally not a fallback in this path.

The shared physical capacity is `64^3 = 262144` bricks. Each cascade receives:

```text
EffectiveMaxScatterBricks = min(ConfigMaxScatterBricks, 262144 / CascadeCount)
PhysicalBrickBase = CascadeIndex * EffectiveMaxScatterBricks
physicalBrickId = PhysicalBrickBase + localAllocatedBrickId
```

`physicalBrickId` alone determines SDF atlas offset. Cascade id is only used to select the logical brick-map and hierarchy slices.

## Build Flow

SDF rebuild runs each cascade sequentially:

```text
for cascade in fine -> coarse:
    init cascade slice and shared buffers
    prepare scatter jobs using cascade bounds
    scan jobs
    mark touched logical bricks
    allocate local bricks into the cascade fixed partition
    scatter SDF samples into temporary scratch
    finalize into shared SDF atlas and physical metadata
    build bottom/top trace hierarchy slices
```

Scatter scratch is recreated as transient RenderGraph resources per cascade pass chain and sized to `EffectiveMaxScatterBricks * 512` uints.

## Trace Flow

Hierarchical trace is fine-to-coarse. Multi-cascade trace uses a local iteration limit of `32` per cascade. `CascadeCount=1` keeps a `64` iteration budget to avoid a single-cascade quality regression while there is no coarser fallback.

If a fine cascade reaches the local iteration limit, trace does not fail immediately. It resumes in the next coarser cascade. Only the last cascade reports `SPARSE_SDF_GI_TRACE_STATUS_ITER_LIMIT`.

The hierarchy remains:

```text
16-brick top cell -> 4-brick bottom cell -> 1 logical brick -> brick-local AABB -> inner SDF march
```

Current v1 caveat: the fine-to-coarse loop advances to the end of each intersected cascade. With the current scene-sized base bounds this is effectively dormant, but toroidal clipmap scrolling can expose a gap for rays that start outside the fine cascade and inside a coarse shell. The toroidal clipmap phase should replace this with segment-wise traversal so the pre-fine-entry interval is traced by the appropriate coarse cascade.

## Runtime Data

`CascadeData` is uploaded through frame-in-flight mapped bindless buffers. Each frame imports its own SRV slot, so CPU updates for changing cascade bounds do not overwrite a buffer that an older GPU frame may still be reading.

Reference stats are accumulated across all rebuilt cascades. Cascade 0 clears the counters during scatter init; each cascade finalize pass adds its job/sample/overflow/occupied-brick counts.

## Validation

Recommended checks:

- `CascadeCount=1/2/4`
- `SDF Atlas Format=Auto/R8/R16`
- Ray Trace, Step Count, Brick ID, Hit UVW, Brick Local Gradient
- large floor/wall triangles
- long thin triangles
- cascade boundary geometry
- long empty-space rays
- screen probe GI

Acceptance criteria:

- no root signature failure
- no invalid bindless index log
- no device removed
- `CascadeCount=4`, `MaxScatterBricks=64K` fits in the shared atlas
- R8 has no large hole or black Brick ID regression
