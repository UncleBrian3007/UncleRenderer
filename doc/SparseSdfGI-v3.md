# SparseSdfGI v3

SparseSdfGI V3 starts moving the dense prototype toward a Brixelizer-shaped build pipeline. V3.1 replaces per-triangle voxel scatter with split reference binning, and V3.2 uses that reference data to solve only occupied bricks during rebuild frames.

## V3 Backbone

V3 is the generation that turns the V2 dense prototype into a Brixelizer-shaped sparse build pipeline. The build path is the focus; tracing/cache (V2.4-V2.6) and overall GI quality are separate concerns.

```text
Done
  V3.1  Split reference binning (triangle pool + brick references) — removes MaxTriangleVoxelSpan holes
  V3.2  Occupied-brick indirect solve dispatch — skips empty-brick no-ops on rebuild frames

Planned (build pipeline, toward Brixelizer parity)
  V3.x  Compacted reference layout (scan/compact contiguous lists, replacing linked lists)
        — better solve locality, removes the per-reference head InterlockedExchange
  V3.x  Triangle pool compression (full f32 anchor + f16 edges, ~24 B/triangle)
  V3.x  Group-local emit reservation (one global atomic per group instead of per element)
        — deprioritized: emit is currently sub-millisecond, only matters for dense/dynamic scenes
  V3.x  Incremental dirty-brick dispatch (re-solve only changed bricks across frames)
        — the Brixelizer dirty model; needs persistent brick state + change tracking; enables dynamic geometry
  V3.x  Sparse brick allocation + free list (real brick pool instead of dense brickId == cell)
  V3.x  Multi-cascade clipmap with scrolling / wrap
  V3.x  Static/dynamic instance handling and merge bricks/cascades
```

### Status and priorities

As of V3.2 the build is fast and complete for static scenes: no holes, trace ~0.8 ms steady state, rebuild ~5-8 ms and cached (only on force rebuild / scene or config change). So the remaining build-pipeline items are driven by two goals, not steady-state cost:

- Scale and dynamics: incremental dirty dispatch, sparse allocation, and multi-cascade are what a large or animated scene needs. Static single-cascade prototypes do not benefit.
- Memory: compacted references and triangle compression reduce the transient build footprint.

The largest user-visible gap is not in this build pipeline at all but in GI quality: the diffuse trace is still an occlusion/AO placeholder rather than real bounced radiance. That work (hit radiance, radiance cache) is tracked separately from the V3 build backbone and can proceed independently of the items above.

## Radiance v1 Diffuse Irradiance

Radiance v1 replaces the old AO-shaped diffuse trace with a first-bounce irradiance signal. `CSDiffuseTrace` now stores irradiance in the SparseSdfGI output texture; it does not multiply by the receiver albedo. The deferred composite path applies the common diffuse contract:

```text
indirect diffuse = irradiance * albedo * (1 - metallic)
```

This matches the environment and ReSTIR diffuse GI paths, so the `Diffuse Indirect` visualization should show SparseSdfGI tinted by the visible surface albedo rather than by a pre-multiplied trace output.

The v1 hit lighting model is deliberately small:

- Miss rays sample the renderer environment cube directly, matching the ray tracing sky path.
- Hit rays compute the hit point from the SDF travel distance and estimate a hit normal from the SDF gradient.
- Hit irradiance combines environment-cube sky lighting along the SDF normal with a directional-light diffuse bounce approximation.
- `SparseSdfGIBounceStrength` scales the SparseSdfGI irradiance contribution.
- `bSparseSdfGIUseHitLightingVisibility` optionally traces from the hit point toward the directional light. It is disabled by default so light-leak behavior and visibility cost can be inspected separately.

Limitations remain significant: the hit surface material is not looked up, there is no radiance cache, no temporal/spatial denoise, and each pixel still uses a single stochastic diffuse ray. Radiance v1 is a signal-shape milestone, not production-quality GI.

## V3.1 Split Reference Binning

The V3.1 builder removes the `MaxTriangleVoxelSpan` drop path. Large triangles are no longer skipped; instead, each static mesh triangle is stored once in a transient triangle pool and lightweight brick references point to that triangle.

```text
CSInitReferenceBuild
  -> clear SDF atlas, brick metadata, dense brick map, reference heads, counters
  -> CSEmitTriangleReferences per static regular mesh
  -> CSSolveBrickReferences
  -> trace/debug reads SdfAtlas + CascadeBrickMap + BrickMetadata
```

The emit pass expands triangle bounds by `SurfaceThicknessVoxels * VoxelSize` before converting to brick coordinates. This is required so triangles near a brick boundary seed neighboring bricks as well. The solve pass gathers each brick's references, computes per-voxel narrow-band seeds, runs the existing brick-local Eikonal relaxation, and writes brick metadata with the same surface threshold used by tracing.

## V3.2 Occupied-Brick Indirect Solve

V3.2 removes the dense `64^3` solve dispatch from rebuild frames. Reference emit now appends a brick to `OccupiedBrickList` only when a valid reference first touches that brick. The first-touch test reuses the linked-list head push: if `InterlockedExchange(referenceHeads[brick], referenceId, oldHead)` returns `INVALID`, the brick has just received its first valid reference.

```text
CSInitReferenceBuild
  -> clear SDF atlas, brick metadata, dense brick map, reference heads, counters
  -> CSEmitTriangleReferences per static regular mesh
  -> CSPrepareSolveBrickReferencesArgs
  -> ExecuteIndirect CSSolveBrickReferences over OccupiedBrickList
  -> trace/debug reads SdfAtlas + CascadeBrickMap + BrickMetadata
```

This is a from-scratch rebuild optimization: empty bricks keep the `SdfAtlas = 1.0` and `BrickMetadata = 0` values written by the init pass, while occupied bricks run the same gather, Eikonal relaxation, and metadata reduction as V3.1. It is also the first step toward Brixelizer-style dirty-brick dispatch; true incremental dirty tracking is future work.

## Resources

- `TrianglePool`: one uncompressed world-space triangle entry per emitted triangle.
- `BrickReferenceHeads`: one linked-list head per dense brick.
- `BrickReferences`: lightweight nodes containing triangle id and next pointer.
- `ReferenceCounters`: triangle count, reference count, triangle overflow, reference overflow, occupied brick count.
- `OccupiedBrickList`: transient dense-brick indices with at least one valid reference in the current rebuild.
- `SolveIndirectArgs`: transient dispatch arguments built from occupied brick count.

V3.1/V3.2 use linked lists for simplicity. A compacted reference layout, triangle compression, and true incremental dirty-brick dispatch are deferred until later V3 work.

## Limitations

- Dense single cascade only.
- Triangle pool is uncompressed; Brixelizer-style compressed triangle storage is future work.
- Reference overflow is a correctness loss and should be treated as a budget problem.
- Static SDF cache still controls steady-state cost; V3.2 primarily reduces rebuild hitch, not cache-hit trace cost.

## References / Attribution

- Inspired by AMD FidelityFX Brixelizer / Brixelizer GI structure.
- No FidelityFX SDK source, shader code, or data tables are copied.
