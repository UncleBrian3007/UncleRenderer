# SparseSdfGI Eikonal Propagation v1

## Summary

- After distributed scatter, run brick-local eikonal relaxation on `ScatterBrickSdf` to propagate the band-limited unsigned distance field into a more conservative field.
- v1 targets all allocated scatter bricks in the current full rebuild and fixed physical partition model.
- Free-lists, dirty brick lists, persistent eikonal counters, and toroidal clipmap dirty tracking are deferred.

## Build Path

Exact Shared Border SDF rebuild order:

1. Clear scatter brick storage
2. Scatter SDF samples
3. Eikonal scatter bricks
4. Finalize scatter bricks
5. Build trace hierarchy

`CSEikonalScatterBricks` reuses `ScatterBrickDispatchArgs`. One thread group processes one scatter physical brick, and 512 threads map one-to-one to the 8x8x8 local samples.

## Shader Policy

- Reuse `DecodeScatterDistanceUint` and `EncodeScatterDistanceUint`.
- Relaxation schedule is `3 sweeps * offsets {4, 2, 1}`.
- Offset 8 is omitted because it is always out of bounds for an 8x8x8 brick.
- Cell distance uses `offset / GetBrickIntervalResolution()` so Exact Shared Border uses the 7-interval spacing.
- Scratch write-back is a plain per-thread store. No interlocked operation is required.

## Config

- Default: `SparseSdfGIEikonalEnabled=true`.
- UI checkbox: `SDF Eikonal Propagation`.
- The enabled flag is part of the SDF build settings signature, so toggling it uses the existing rebuild and radiance/probe history clear path.

## Validation Notes

- Eikonal is brick-local. Duplicated shared-border samples can relax from different brick interiors and diverge numerically.
- `Shared Sample Mismatch` may therefore increase with eikonal enabled.
- Treat this as a failure only if it becomes visible as a gradient seam, hit-position discontinuity, or GI artifact.
- If visible seams appear, v2 should add a border min-merge pass using the shared-border pair machinery.

## Tuning Policy

v1 does not change trace constants. Capture eikonal on/off quality and timing first, then compare these candidates separately:

- Leaf inner max step distance: `2.0 * voxelSize -> 4.0 * voxelSize`
- `SPARSE_SDF_GI_TRACE_HIERARCHY_INNER_STEPS`: `8 -> 6 -> 4`

## Test Plan

- Compile `CSEikonalScatterBricks`, `CSScatterSdfSamples`, `CSFinalizeScatterBricks`, `CSDebugTrace`, `CSDiffuseTrace`, `CSTraceScreenProbes`.
- Run Debug x64 MSBuild.
- Compare SparseSdfGI rebuild GPU timing with eikonal on/off.
- Compare Step Count debug before and after eikonal before changing trace constants.
- Test `CascadeCount=1/2/4` and `SDF Atlas Format=Auto/R8/R16`.
