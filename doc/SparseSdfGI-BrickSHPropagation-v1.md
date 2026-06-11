# SparseSdfGI Brick SH Propagation v1

## Summary

- Brick radiance/irradiance cache is stored as L1 SH instead of `float4(rgb, confidence)`.
- The SH layout uses `CommonSH.hlsli` `FPackedSh`: luminance L1 SH plus DC chroma.
- Propagation is an FFX BrixelizerGI-style 26-neighbor smoothing pass over allocated logical bricks.
- v1 does not allocate missing logical bricks, so this is not sparse hole allocation. It fills and stabilizes low-sample allocated bricks from valid neighbors.

## Cache Semantics

- `FBrickShGpu` is 32 bytes: packed SH `uint4`, `SampleCount`, and padding.
- `FBrickShAccumGpu` is 32 bytes and accumulates signed SH coefficients atomically before resolve.
- `SPARSE_SDF_GI_RADIANCE_HISTORY_DECAY` remains `0.985`.
- Confidence is `saturate(SampleCount / 64)`.
- The existing `SPARSE_SDF_GI_RADIANCE_CONFIDENCE_THRESHOLD = 0.05` maps to a minimum valid sample count of 4.

## Read-Side Evaluation

- GI ray bounce reads brick radiance with the hit normal when the SDF gradient is reliable.
- If the SDF gradient is weak and the normal falls back to `-traceDirection`, cache evaluation uses `ApproxRadiance` instead of directional `UnprojectIrradiance`.
- Multi-bounce inject reads brick irradiance with the GBuffer normal and falls back to DC only when the normal is invalid.

## Propagation

- Dispatch covers `CascadeCount * 64^3` logical bricks.
- Base logical brick must be allocated; otherwise the thread returns immediately.
- Base weight is `SampleCount^2`.
- Valid neighbors are sampled from the same cascade's 3x3x3 logical brick neighborhood, excluding center.
- Neighbor weight is `1 / distanceSq`.
- Direction gating uses normalized directionality: `length(ShY.yzw) / max(abs(ShY.x), eps)`.
- Gating only applies when both base and neighbor are directional enough.

## Notes

- Memory is about 8 MiB per SH cache buffer at `262144 * 32B`.
- Radiance history slots, irradiance slots, and accum buffers are roughly 2x the old `float4` cache footprint.
- Effective propagation cost is closer to allocated brick count times 26 taps than to the dispatch upper bound, because unallocated base bricks early out.
- Future half-resolution GI must keep the order `Upsample -> Irradiance Accumulate/Resolve/Propagate`.
