# SparseSdfGI v2

SparseSdfGI v2 moves the experiment from a triangle narrow-band shell toward a traceable scene-space distance field. It is still an original UncleRenderer implementation inspired by AMD FidelityFX Brixelizer and Brixelizer GI, without copying SDK source, shader code, or data tables.

## Current v2.0 Scope

- Fit the dense single cascade to the loaded scene instead of using a fixed world-space voxel size by default.
- Treat `SparseSdfGIBaseVoxelSize <= 0` as auto mode.
- Compute auto voxel size from scene radius:

```text
voxelSize = max((2 * SceneRadius * 1.10) / 512, 0.001)
```

- Keep positive `SparseSdfGIBaseVoxelSize` values as manual voxel-size overrides.
- Feed the effective voxel size consistently to clear, voxelize, trace, debug, and diffuse passes.

This fixes the v1 failure mode where small scenes occupied only a few voxels inside a very large 512^3 cascade.

## Current v2.1 Scope

- Add a `R32_UINT` seed distance atlas beside the existing filterable SDF atlas.
- Clear seed voxels to `0xffffffff` and use `InterlockedMin` during triangle voxelization.
- Quantize the narrow-band unsigned triangle distance to 16 bits before storing it in the seed atlas.
- Resolve seed distances back into the existing normalized `R16_UNORM` SDF atlas so debug visualization and trace code can keep sampling the same filterable resource.

V2.1 fixes the v1 last-writer-wins problem when multiple triangles touch the same voxel. It does not yet propagate distances through empty space, so the field is still a surface seed band rather than a full global SDF.

## Current v2.2 Scope

- Move the `R32_UINT` seed distance atlas to a transient RenderGraph 3D texture used only during SDF build.
- Store seed distances in brick-scale normalized units: `1.0 == 8 * voxelSize`.
- Replace the seed resolve pass with a Brixelizer-style brick-local Eikonal pass.
- Solve every dense single-cascade brick each frame: one compute group per `8^3` brick, one thread per brixel.
- Write the full `R16_UNORM` SDF atlas every frame, including empty bricks as `1.0`.

V2.2 improves the distance field inside each brick. It does not propagate across brick boundaries, so 8^3 brick seams can still appear until brick-aware traversal, AABB skipping, or an optional GlobalSDF-style propagation path is added.

## Long-Term v2 Backbone

```text
V2.0  SceneRadius-based cascade / voxel-size auto fit
V2.1  R32_UINT atomic-min seed distance atlas
V2.2  Transient seeds + brick-scale brick-local Eikonal
V2.3  Brick-aware distance / step-count debug views
V2.4  Use cascade brick-map indirection in sampling and traversal
V2.5  Static SDF cache / rebuild skip
V2.6  Occupied-brick Eikonal dispatch skip
Later Trace brick AABB metadata and empty-brick / empty-space skip
Optional  GlobalSDF-style inter-brick propagation or packed JFA debug path
```

V2 stays a dense single-cascade prototype. V2.4 is the first hard prerequisite for going Brixelizer-shaped: sampling must route through the brick map instead of the flat world->atlas mapping. V2.5 prioritizes measured cost: static scenes reuse the persistent SDF atlas instead of rebuilding it every frame. V2.6 should reduce rebuild frames by dispatching Eikonal only for occupied bricks.

The sparse allocator and the rest of the Brixelizer build pipeline are grouped into V3 because they are mutually dependent and only pay off together.

### V2.1 Atomic-Min Seeds

The v1 atlas is `RWTexture3D<float>` and accepts last-writer-wins results when multiple triangles touch the same voxel. V2.1 adds a seed distance atlas using `R32_UINT` and clears it to `0xffffffff`. Voxelization quantizes world-space triangle distance and uses `InterlockedMin` so the nearest triangle wins.

The seed pass remains unsigned. Inside/outside classification is outside v2 scope.

The resolved atlas uses `R16_UNORM` instead of `R8_UNORM` so the 16-bit seed quantization survives into the filterable SDF atlas. This matters more once tracing steps directly from decoded distance values.

### V2.2 Brick-Local Eikonal

Voxelization seeds only a small surface band controlled by `SurfaceThicknessVoxels`. The stored value is normalized by brick width, not by seed band width:

```text
sdf = distanceToTriangle / (8 * voxelSize)
```

The Eikonal pass loads one `8^3` brick into LDS, uses deterministic ping-pong relaxation with offsets `4, 2, 1`, and writes the final brick-local unsigned field to the filterable SDF atlas. Empty seeds remain `1.0`.

The earlier "narrow seed band" milestone is folded into V2.2: voxelization still emits a narrow surface seed band, but distance expansion inside each brick is handled by the brick-local Eikonal pass.

### V2.3 Brick-Aware Debug

`Voxel Projection` remains a raw occupancy/debug view. The old `Global SDF Surface` debug mode is renamed to `Brick SDF Surface` because the current field is a dense cascade of brick-local Eikonal solves, not a continuous UE-style GlobalSDF.

V2.3 adds a `Step Count` debug mode that reuses the brick surface trace path and visualizes traversal cost. Hit pixels are shown in grayscale, with brighter values meaning more steps. Miss pixels stay dark, with subtle color differences for cascade miss, max-distance stop, cascade exit, and atlas-boundary escape. This mode is intended to reveal brick seams, conservative step clamps, empty-brick behavior, and trace-cost hot spots before adding brick-map indirection or AABB skipping.

### V2.4 Brick-Map Indirection

V2.4 routes all SDF sampling and tracing through the cascade brick map before reading the SDF atlas. Dense mode still initializes `cascadeBrickMap[linearBrick] = linearBrick`, so visual output should stay close to V2.3 while proving that the Brixelizer-style indirection path is correct.

Filtered SDF reads use manual trilinear sampling: each of the eight neighbor voxels is resolved through `CascadeBrickMap -> brickId -> atlas brick base`. This is intentionally more expensive than hardware `SampleLevel`, but it makes invalid/sparse bricks observable and gives V2.5 a clear Step Count baseline. Cascade outside, invalid brick ids, and atlas outside all return `1.0` empty distance instead of clamp-to-edge.

### V2.5 Static SDF Cache

V2.5 skips `Seed Atlas Init`, per-model voxelize, and `Brick Eikonal` on frames where the static scene and build settings are unchanged. `SdfAtlas` and `CascadeBrickMap` are persistent resources, so cache-valid frames keep importing them and trace/debug/diffuse still read the previous build output.

The cache signature includes the actual cascade bounds, build-affecting config, static regular mesh candidate count, draw ranges, vertex/index SRV bindless indices, and world matrices. Debug mode, intensity, camera movement, and trace half-resolution do not invalidate the cache. Vertex/index buffer contents are assumed immutable for a given SRV bindless index; in-place streaming or morph updates are outside V2.5.

A `Force Rebuild SDF` ImGui button invalidates the cache without writing config. Runtime validation should confirm that Sponza shows build passes on the first frame or after forced invalidation, then only `SparseSdfGI Trace` on steady-state frames.

### V2.6 Occupied-Brick Eikonal Dispatch

The next measured target is rebuild-frame cost. Seeded or occupied brick metadata should be used to dispatch Eikonal only for bricks touched by voxelization. This attacks the current largest per-frame cost when a rebuild is required.

### Later Brick AABB Trace Skip

After rebuild-frame cost is reduced, add brick metadata for ray traversal so trace can skip invalid, empty, or conservatively bounded bricks. This remains useful, but current measurements show trace is much cheaper than full SDF rebuild.

### Optional GlobalSDF-Style Propagation

Packed Jump Flooding or another inter-brick propagation path remains useful if the goal shifts toward GlobalSDF visualization. It is optional for the Brixelizer-style path and should be treated as a debug/alternate mode because packed seed volumes can cost about 512 MB each at 512^3.

## V3 - Sparse / Multi-Cascade Generation

V3 turns the dense prototype into a Brixelizer-shaped sparse renderer. These items are mutually dependent: the sparse allocator is fed by the reference pipeline and only pays off across multiple cascades, so they ship as one generation rather than separate milestones.

```text
V3.x  Sparse brick allocation, free list, and dirty-brick updates
V3.x  Reference-binning voxelizer (replaces the per-triangle bbox loop)
V3.x  Multi-cascade clipmap with scrolling / wrap
V3.x  Static/dynamic instance handling and merge bricks/cascades
V3.x  Incremental rebuild of dirty bricks only
```

The reference-binning voxelizer is the structural fix for the `MaxTriangleVoxelSpan` guard: Brixelizer never drops large triangles, it clamps their voxel AABB to the cascade and bounds total work with a global reference budget. Adopting it removes the span guard entirely, but it is coupled to sparse allocation and multi-cascade, so it belongs in V3 rather than V2.

## Deferred Work

- Signed SDF / inside-outside classification.
- Global Eikonal, Fast Sweeping, or Fast Marching quality solve.
- Radiance cache, specular GI, and production GI denoising.

## References / Attribution

- AMD FidelityFX SDK, Brixelizer / Brixelizer GI
- Copyright (C) 2024 Advanced Micro Devices, Inc.
- Licensed under the MIT License

If SDK code, HLSL, constants, or substantial portions are copied later, add a `ThirdPartyNotices.md` entry with the full MIT license text and the copied scope.
